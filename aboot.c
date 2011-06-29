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

#include <errno.h>
#include <fcntl.h>
#include <linux/ext3_fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cutils/log.h>
#include <diskconfig/diskconfig.h>

#include "fastboot.h"
#include "droidboot.h"
#include "util.h"
#include "update_osip.h"

#define CMD_SYSTEM		"system"
#define CMD_PARTITION		"partition"

#define EXT_SUPERBLOCK_OFFSET	1024

/* Erase a named partition by creating a new empty partition on top of
 * its device node. No parameters. */
static void cmd_erase(const char *part_name, void *data, unsigned sz)
{
	struct part_info *ptn;
	char *pdevice = NULL;
	char *cmd = NULL;

	LOGI("%s: %s\n", __func__, part_name);
	ptn = find_part(disk_info, part_name);
	if (ptn == NULL) {
		fastboot_fail("unknown partition name");
		return;
	}

	LOGD("Erasing %s.\n", part_name);

	pdevice = find_part_device(disk_info, ptn->name);
	if (!pdevice) {
		fastboot_fail("find_part_device failed!");
		die();
	}
	LOGV("destination device: %s\n", pdevice);
	if (!is_valid_blkdev(pdevice)) {
		fastboot_fail("invalid destination node. partition disks?");
		goto out;
	}

	switch (ptn->type) {
	case PC_PART_TYPE_LINUX:
		if (asprintf(&cmd, "/system/bin/make_ext4fs -L %s %s",
					ptn->name, pdevice) < 0) {
			fastboot_fail("memory allocation error");
			cmd = NULL;
			goto out;
		}
		if (execute_command(cmd)) {
			fastboot_fail("make_ext4fs failed");
			goto out;
		}
		break;
	default:
		fastboot_fail("Unsupported partition type");
		goto out;
	}

	fastboot_okay("");
out:
	if (pdevice)
		free(pdevice);
	if (cmd)
		free(cmd);
}

/* Image command. Allows user to send a single gzipped file which
 * will be decompressed and written to a destination location. Typical
 * usage is to write to a disk device node, in order to flash a raw
 * partition, but can be used to write any file.
 *
 * The parameter part_name can be one of several possibilities:
 *
 * "disk" : Write directly to the disk node specified in disk_layout.conf,
 *          whatever it is named there.
 * "osipX" : MFLD only, X is some integer. update OS image with a
 *           stitched OS image. The provided image must have exactly
 *           one OSII record in it.
 * <name> : Lookup the named partition in disk_layout.conf and write to
 *          its corresponding device node
 */
static void cmd_flash(const char *part_name, void *data, unsigned sz)
{
	FILE *fp = NULL;
	char *cmd = NULL;
	int ret;
	char *device;
	char *cmd_base;
	struct part_info *ptn = NULL;
	unsigned char *data_bytes = (unsigned char *)data;
	int free_device = 0;
	int do_ext_checks = 0;

	if (!strcmp(part_name, "disk")) {
		device = disk_info->device;
	} else if (!strncmp(part_name, "osip", 4)) {
		/* Rest of the part name is the index number */
		int entry_index = atoi(part_name + 4);
		LOGI("Update OSIP entry %d\n", entry_index);
		if (write_stitch_image(data, sz, entry_index))
			fastboot_fail("write_stitch_image failure");
		else
			fastboot_okay("");
		return;
	} else {
		free_device = 1;
		device = find_part_device(disk_info, part_name);
		if (!device) {
			fastboot_fail("unknown partition specified");
			return;
		}
		ptn = find_part(disk_info, part_name);
	}

	LOGD("Writing %u bytes to destination device: %s\n", sz, device);
	if (!is_valid_blkdev(device)) {
		fastboot_fail("invalid destination node. partition disks?");
		goto out;
	}

	/* Check for a gzip header, and use gzip to decompress if present.
	 * See http://www.gzip.org/zlib/rfc-gzip.html#file-format */
	if (sz > 2 && data_bytes[0] == 0x1f &&
			data_bytes[1] == 0x8b && data_bytes[3] == 8) {
		cmd_base =
		    "/system/bin/gzip -c -d | /system/bin/dd of=%s bs=8192";
	} else {
		cmd_base = "/system/bin/dd of=%s bs=8192";
	}

	if (asprintf(&cmd, cmd_base, device) < 0) {
		LOGPERROR("asprintf");
		cmd = NULL;
		fastboot_fail("memory allocation error");
		goto out;
	}

	LOGV("command: %s\n", cmd);
	fp = popen(cmd, "w");
	if (!fp) {
		LOGPERROR("popen");
		fastboot_fail("popen failure");
		goto out;
	}
	free(cmd);
	cmd = NULL;

	if (sz != fwrite(data, 1, sz, fp)) {
		LOGPERROR("fwrite");
		fastboot_fail("image write failure");
		goto out;
	}
	pclose(fp);
	fp = NULL;
	sync();

	LOGD("wrote %u bytes to %s\n", sz, device);

	/* Check if we wrote to the base device node. If so,
	 * re-sync the partition table in case we wrote out
	 * a new one */
	if (!strcmp(device, disk_info->device)) {
		int fd = open(device, O_RDWR);
		if (fd < 0) {
			fastboot_fail("could not open device node");
			goto out;
		}
		LOGV("sync partition table\n");
		ioctl(fd, BLKRRPART, NULL);
		close(fd);
	}

	/* Make sure this is really an ext4 partition before we try to
	 * run some disk checks and resize it, ptn->type isn't sufficient
	 * information */
	if (ptn && ptn->type == PC_PART_TYPE_LINUX) {
		/* EXT4 uses same superblock struct and s_magic */
		struct ext3_super_block superblock;
		int fd = open(device, O_RDWR);
		if (fd < 0) {
			fastboot_fail("could not open device node");
			goto out;
		}
		if (lseek(fd, EXT_SUPERBLOCK_OFFSET, SEEK_SET) !=
				EXT_SUPERBLOCK_OFFSET) {
			fastboot_fail("could not seek to superblock offset");
			close(fd);
			goto out;
		}
		if (read(fd, &superblock, sizeof(superblock)) != sizeof(superblock)) {
			fastboot_fail("couldn't read superblock");
			close(fd);
			goto out;
		}
		close(fd);
		if (EXT3_SUPER_MAGIC == superblock.s_magic) {
			do_ext_checks = 1;
		}
	}
	if (do_ext_checks) {
		/* Resize the filesystem to fill the partition */
		if (asprintf(&cmd, "/system/bin/resize2fs -F %s",
					device) < 0) {
			cmd = NULL;
			fastboot_fail("memory allocation error");
			goto out;
		}
		if (execute_command(cmd)) {
			fastboot_fail("could not resize filesystem "
					"to fill disk");
			goto out;
		}
		free(cmd);
		cmd = NULL;

		/* run fdisk to make sure the partition is OK */
		if (asprintf(&cmd, "/system/bin/e2fsck -C 0 -fy %s",
					device) < 0) {
			cmd = NULL;
			fastboot_fail("memory allocation error");
			goto out;
		}
		ret = execute_command(cmd);
		if (ret < 0 || ret > 1) {
			/* Return value of 1 is OK */
			fastboot_fail("fsck of filesystem failed");
			goto out;
		}
		free(cmd);
		cmd = NULL;

		/* Set mount count to 1 so that 1st mount on boot doesn't
		 * result in complaints */
		if (asprintf(&cmd, "/system/bin/tune2fs -C 1 %s",
					device) < 0) {
			cmd = NULL;
			fastboot_fail("memory allocation error");
			goto out;
		}
		if (execute_command(cmd)) {
			fastboot_fail("tune2fs failed");
			goto out;
		}
		free(cmd);
		cmd = NULL;
	}

	fastboot_okay("");
out:
	if (fp)
		pclose(fp);
	if (cmd)
		free(cmd);
	if (device && free_device)
		free(device);
}

static void cmd_oem(const char *arg, void *data, unsigned sz)
{
	const char *command;
	LOGV("%s: <%s>\n", __FUNCTION__, arg);

	while (*arg == ' ')
		arg++;
	command = arg;

	if (strncmp(command, CMD_SYSTEM, strlen(CMD_SYSTEM)) == 0) {
		int retval;
		arg += strlen(CMD_SYSTEM);
		while (*arg == ' ')
			arg++;
		retval = execute_command(arg);
		if (retval != 0) {
			LOGE("\nfails: %s (return value %d)\n", arg, retval);
			fastboot_fail("OEM system command failed");
		} else {
			LOGV("\nsucceeds: %s\n", arg);
			fastboot_okay("");
		}
	} else if (strncmp(command, CMD_PARTITION,
				strlen(CMD_PARTITION)) == 0) {
		LOGI("Applying disk configuration\n");
		if (apply_disk_config(disk_info, 0))
			fastboot_fail("apply_disk_config error");
		else
			fastboot_okay("");
	} else {
		fastboot_fail("unknown OEM command");
	}
	return;
}

static void cmd_boot(const char *arg, void *data, unsigned sz)
{
	fastboot_fail("boot command stubbed on this platform!");
}

static void cmd_reboot(const char *arg, void *data, unsigned sz)
{
	fastboot_okay("");
	sync();
	LOGI("Rebooting!\n");
	/* The "android" parameter is recognized on MFLD devices
	 * as a directive to the OSIP driver to un-corrupt the OSIP
	 * header so that the Android kernel will be started by the FW
	 * instead of droidboot.  Other devices ignore it. */
	__reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
			LINUX_REBOOT_CMD_RESTART2, "android");
	LOGE("Reboot failed");
}

static void cmd_continue(const char *arg, void *data, unsigned sz)
{
	start_default_kernel();
	fastboot_fail("Unable to boot default kernel!");
}

void aboot_register_commands(void)
{
	fastboot_register("oem", cmd_oem);
	fastboot_register("boot", cmd_boot);
	fastboot_register("reboot", cmd_reboot);
	fastboot_register("erase:", cmd_erase);
	fastboot_register("flash:", cmd_flash);
	fastboot_register("continue", cmd_continue);

	fastboot_publish("product", DEVICE_NAME);
	fastboot_publish("kernel", "droidboot");
}
