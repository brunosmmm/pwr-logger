#include "storage.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <ff.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(storage, LOG_LEVEL_INF);

#define DISK_VOLUME "SD"
#define MOUNT_POINT "/" DISK_VOLUME ":"

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = MOUNT_POINT,
};

int storage_mount(void)
{
	int rc = fs_mount(&mp);

	if (rc != 0) {
		LOG_ERR("fs_mount(%s) failed: %d", MOUNT_POINT, rc);
	} else {
		LOG_INF("mounted %s", MOUNT_POINT);
	}
	return rc;
}

int storage_unmount(void)
{
	return fs_unmount(&mp);
}

int storage_session_begin(struct storage_file *f, const char *name)
{
	char path[80];

	snprintf(path, sizeof(path), "%s/%s", MOUNT_POINT, name);
	fs_file_t_init(&f->fp);

	int rc = fs_open(&f->fp, path, FS_O_CREATE | FS_O_WRITE);

	if (rc != 0) {
		LOG_ERR("fs_open(%s) failed: %d", path, rc);
		return rc;
	}
	/* TODO: f_expand() a contiguous extent here and track valid length so
	 * writes avoid per-write FAT updates (REQUIREMENTS.md section 4). */
	LOG_INF("session file open: %s", path);
	return 0;
}

int storage_write(struct storage_file *f, const void *buf, size_t len)
{
	ssize_t w = fs_write(&f->fp, buf, len);

	if (w < 0) {
		return (int)w;
	}
	return ((size_t)w == len) ? 0 : -EIO;
}

int storage_sync(struct storage_file *f)
{
	return fs_sync(&f->fp);
}

int storage_session_end(struct storage_file *f)
{
	return fs_close(&f->fp);
}
