/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name of Google, Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#define LOG_TAG "droidboot"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mount.h>

#include <minui/minui.h>
#include <cutils/android_reboot.h>
#include <cutils/klog.h>
#include <charger/charger.h>
#include <diskconfig/diskconfig.h>

#include "aboot.h"
#include "droidboot_util.h"
#include "droidboot.h"
#include "fastboot.h"
#include "droidboot_ui.h"
#include "droidboot_fstab.h"

/* Generated by the makefile, this function defines the
 * RegisterDeviceExtensions() function, which calls all the
 * registration functions for device-specific extensions. */
#include "register.inc"

/* NOTE: Droidboot uses two sources of information about the disk. There
 * is disk_layout.conf which specifies the physical partition layout on
 * the disk via libdiskconfig. There is also recovery.fstab which gives
 * detail on the filesystems associates with these partitions, see fstab.c.
 * The device node is used to link these two sources when necessary; the
 * 'name' fields are typically not the same.
 *
 * It would be best to have this in a single data store, but we wanted to
 * leverage existing android mechanisms whenever possible, there are already
 * too many different places in the build where filesystem data is recorded.
 * So there is a little bit of ugly gymnastics involved when both sources
 * need to be used.
 */

/* libdiskconfig data structure representing the intended layout of the
 * internal disk, as read from /etc/disk_layout.conf */
struct disk_info *disk_info;

/* Synchronize operations which touch EMMC. Fastboot holds this any time it
 * executes a command. Threads which touch the disk should do likewise. */
pthread_mutex_t action_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Not bothering with concurrency control as this is just a flag
 * that gets cleared */
static int autoboot_enabled;

/* Whether to kexec into a 2nd-stage kernel on boot */
static int g_use_autoboot = 0;

/* Filesystem containing 2nd-stage boot images */
static char *g_2ndstageboot_part = DATA_PTN;

/* Directory within filesystem containing 2nd-stage boot images */
static char *g_2ndstageboot_dir = "2ndstageboot";

/* When performing a countdown, how many seconds to wait */
static int g_autoboot_delay_secs = 8;

/* Default size of memory buffer for image data */
static int g_scratch_size = 400;

/* Minimum battery % before we do anything */
static int g_min_battery = 10;

/* If nonzero, wait for "fastboot continue" before applying a
 * detected SW update in try_update_sw() */
static int g_update_pause = 0;

char *g_update_location = NULL;

#define AUTO_UPDATE_FNAME	DEVICE_NAME ".auto-ota.zip"

int (*platform_provision_function)(void);

void set_platform_provision_function(int (*fn)(void))
{
	platform_provision_function = fn;
}

/* Set up a specific partition in preparation for auto-update The source_device
 * is the partition that the update is stored on; if it's the same
 * as the partition that we're performing this routine on, verify its
 * integrity and resize it instead of formatting.
 *
 * Note that erase_partition does a 'quick' format; the disk is not zeroed
 * out. */
static int provision_partition(const char *name, Volume *source_volume)
{
	struct part_info *ptn;
	char *device = NULL;
	int ret = -1;

	/* Set up cache partition */
	ptn = find_part(disk_info, name);
	if (!ptn) {
		pr_error("Couldn't find %s partition. Is your "
				"disk_layout.conf valid?", name);
		goto out;
	}
	device = find_part_device(disk_info, ptn->name);
	if (!device) {
		pr_error("Can't get %s partition device node!", name);
		goto out;
	}
	/* Not checking device2; if people are declaring multiple devices
	 * for cache and data, they're nuts */
	if (!strcmp(source_volume->device, device)) {
		if (ext4_filesystem_checks(device, ptn)) {
			pr_error("%s filesystem corrupted", name);
			goto out;
		}
	} else {
		if (erase_partition(ptn)) {
			pr_error("Couldn't format %s partition", name);
			goto out;
		}
	}

	ret = 0;
out:
	free(device);
	return ret;
}

/* Ensure the device's disk is set up in a sane way, such that it's possible
 * to apply a full OTA update */
static int provisioning_checks(Volume *source_device)
{
	pr_debug("Preparing device for provisioning...");

	if (platform_provision_function && platform_provision_function()) {
		pr_error("Platform-speciifc provision function failed");
		return -1;
	}

	if (provision_partition(CACHE_PTN, source_device)) {
		return -1;
	}

	if (provision_partition(DATA_PTN, source_device)) {
		return -1;
	}

	return 0;
}

/* Check a particular volume to see if there is an automatic OTA
 * package present on it, and if so, return a path which can be
 * fed to the command line of the recovery console.
 *
 * Don't report errors if we can't mount the volume or the
 * auto-ota file doesn't exist. */
static char *detect_sw_update(Volume *vol)
{
	char *filename = NULL;
	struct stat statbuf;
	char *ret = NULL;
	char *mountpoint = NULL;

	if (asprintf(&mountpoint, "/mnt%s", vol->mount_point) < 0) {
		pr_perror("asprintf");
		die();
	}

	if (asprintf(&filename, "%s/" AUTO_UPDATE_FNAME,
				mountpoint) < 0) {
		pr_perror("asprintf");
		die();
	}
	pr_debug("Looking for %s...", filename);

	if (mount_partition_device(vol->device, vol->fs_type,
			mountpoint)) {
		if (!vol->device2 || mount_partition_device(vol->device2,
				vol->fs_type, mountpoint))
		{
			pr_debug("Couldn't mount %s", vol->mount_point);
			goto out;
		}
	}

	if (stat(filename, &statbuf)) {
		if (errno == ENOENT)
			pr_debug("Coudln't find %s", filename);
		else
			pr_perror("stat");
	} else {
		ret = strdup(filename + 4); /* Nip off leading '/mnt' */
		if (!ret) {
			pr_perror("strdup");
			die();
		}
		pr_info("OTA Update package found: %s", filename);
	}
out:
	free(filename);
	umount(mountpoint);
	free(mountpoint);
	return ret;
}

void disable_autoboot(void)
{
	if (autoboot_enabled) {
		autoboot_enabled = 0;
		pr_info("Countdown disabled.\n");
	}
}

static int countdown(char *action, int seconds)
{
	int ret;
	autoboot_enabled = 1;
	ui_show_progress(1.0, seconds);
	pr_info("Press a button to cancel this countdown");
	for ( ; seconds && autoboot_enabled; seconds--) {
		pr_info("Automatic %s in %d seconds\n", action, seconds);
		sleep(1);
	}
	ui_reset_progress();
	ret = autoboot_enabled;
	autoboot_enabled = 0;
	return ret;
}

int try_update_sw(Volume *vol, int use_countdown)
{
	int ret = 0;
	char *update_location;

	/* Check if we've already been here */
	if (g_update_location)
		return 0;

	update_location = detect_sw_update(vol);
	if (!update_location)
		return 0;

	if (use_countdown) {
		int countdown_complete;
		ui_show_text(1);
		countdown_complete = countdown("SW update",
				g_autoboot_delay_secs);
		ui_show_text(0);
		if (!countdown_complete) {
			free(update_location);
			return 0;
		}
	}

	ret = -1;

	pthread_mutex_lock(&action_mutex);
	ui_show_indeterminate_progress();
	if (provisioning_checks(vol)) {
		free(update_location);
	} else {
		if (!g_update_pause) {
			apply_sw_update(update_location, 0);
			free(update_location);
		} else {
			/* Stash the location for later use with
			 * 'fastboot continue' */
			g_update_location = update_location;
			ret = 0;
		}
	}
	ui_reset_progress();
	pthread_mutex_unlock(&action_mutex);
	return ret;
}


static void *autoboot_thread(void *arg)
{
	/* FIXME: check if there's anything to actually boot
	 * before starting the countdown */
	if (!countdown("boot", g_autoboot_delay_secs))
		return NULL;

	ui_reset_progress();
	ui_show_text(1);
	start_default_kernel();
	return NULL;
}

static int input_callback(int fd, short revents, void *data)
{
	struct input_event ev;
	int ret;

	ret = ev_get_input(fd, revents, &ev);
	if (ret)
		return -1;

	pr_verbose("Event type: %x, code: %x, value: %x\n",
				ev.type, ev.code,
				ev.value);

	switch (ev.type) {
		case EV_KEY:
			disable_autoboot();
			break;
		default:
			break;
	}
	return 0;
}


static void *input_listener_thread(void *arg)
{
	pr_verbose("begin input listener thread\n");

	while (1) {
		if (!ev_wait(-1))
			ev_dispatch();
	}
	pr_verbose("exit input listener thread\n");

	return NULL;
}


void start_default_kernel(void)
{
	char basepath[PATH_MAX];
	struct part_info *ptn;
	ptn = find_part(disk_info, g_2ndstageboot_part);

	if (mount_partition(ptn)) {
		pr_error("Can't mount second-stage boot partition (%s)\n",
				g_2ndstageboot_part);
		return;
	}

	snprintf(basepath, sizeof(basepath), "/mnt/%s/%s/",
			g_2ndstageboot_part, g_2ndstageboot_dir);
	kexec_linux(basepath);
	/* Failed if we get here */
	pr_error("kexec failed");
}


void setup_disk_information(char *disk_layout_location)
{
	/* Read the recovery.fstab, which is used to for filesystem
	 * meta-data and also the sd card device node */
	load_volume_table();

	/* Read disk_layout.conf, which provides physical partition
	 * layout information */
	pr_debug("Reading disk layout from %s\n", disk_layout_location);
	disk_info = load_diskconfig(disk_layout_location, NULL);
	if (!disk_info) {
		pr_error("Disk layout unreadable");
		die();
	}
	process_disk_config(disk_info);
	dump_disk_config(disk_info);

	/* Set up the partition table */
	if (apply_disk_config(disk_info, 0)) {
		pr_error("Couldn't apply disk configuration");
		die();
	}
}


static void parse_cmdline_option(char *name)
{
	char *value = strchr(name, '=');

	if (value == 0)
		return;
	*value++ = 0;
	if (*name == 0)
		return;

	if (!strncmp(name, "droidboot", 9))
		pr_info("Got parameter %s = %s\n", name, value);
	else
		return;

	if (!strcmp(name, "droidboot.bootloader")) {
		g_use_autoboot = atoi(value);
	} else if (!strcmp(name, "droidboot.delay")) {
		g_autoboot_delay_secs = atoi(value);
	} else if (!strcmp(name, "droidboot.scratch")) {
		g_scratch_size = atoi(value);
	} else if (!strcmp(name, "droidboot.minbatt")) {
		g_min_battery = atoi(value);
	} else if (!strcmp(name, "droidboot.bootpart")) {
		g_2ndstageboot_part = strdup(value);
		if (!g_2ndstageboot_part)
			die();
	} else if (!strcmp(name, "droidboot.bootdir")) {
		g_2ndstageboot_part = strdup(value);
		if (!g_2ndstageboot_part)
			die();
	} else if (!strcmp(name, "droidboot.updatepause")) {
		g_update_pause = atoi(value);
	} else {
		pr_error("Unknown parameter %s, ignoring\n", name);
	}
}


int main(int argc, char **argv)
{
	char *config_location;
	pthread_t t_auto, t_input;
	Volume *vol;


	/* initialize libminui */
	ui_init();


	pr_info(" -- Droidboot %s for %s --\n", DROIDBOOT_VERSION, DEVICE_NAME);
	import_kernel_cmdline(parse_cmdline_option);

#ifdef USE_GUI
	/* Enforce a minimum battery level */
	if (g_min_battery != 0) {
		pr_info("Verifying battery level >= %d%% before continuing\n",
				g_min_battery);
		klog_init();
		klog_set_level(8);

		switch (charger_run(g_min_battery, POWER_ON_KEY_TIME,
					BATTERY_UNKNOWN_TIME,
					UNPLUGGED_SHUTDOWN_TIME,
					CAPACITY_POLL_INTERVAL)) {
		case CHARGER_SHUTDOWN:
			android_reboot(ANDROID_RB_POWEROFF, 0, 0);
			break;
		case CHARGER_PROCEED:
			pr_info("Battery level is acceptable\n");
			break;
		default:
			pr_error("mysterious return value from charger_run()\n");
		}
		ev_exit();
	}
#endif

	ev_init(input_callback, NULL);
	ui_set_background(BACKGROUND_ICON_INSTALLING);

	if (argc > 1)
		config_location = argv[1];
	else
		config_location = DISK_CONFIG_LOCATION;

	setup_disk_information(config_location);

	aboot_register_commands();

	register_droidboot_plugins();

	if (pthread_create(&t_input, NULL, input_listener_thread,
					NULL)) {
		pr_perror("pthread_create");
		die();
	}

	vol = volume_for_path(SDCARD_VOLUME);
	if (vol)
		try_update_sw(vol, 1);

	if (g_use_autoboot && !g_update_location) {
		if (pthread_create(&t_auto, NULL, autoboot_thread, NULL)) {
			pr_perror("pthread_create");
			die();
		}
	}

	pr_info("Listening for the fastboot protocol over USB.");
	fastboot_init(g_scratch_size * MEGABYTE);

	/* Shouldn't get here */
	exit(1);
}
