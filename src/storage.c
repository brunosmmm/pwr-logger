#include "storage.h"
#include "clock.h"
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

/*
 * Write-combining buffer. The TDMS encoder issues many small/odd-sized writes
 * (28 B lead-in, metadata, then a 16 KB raw block), which would land unaligned
 * to the 512 B SD sector and force FatFs into slow read-modify-write on every
 * block. We coalesce all writes into a sector-multiple buffer and only ever
 * call fs_write() with whole sectors at a sector-aligned file offset, so FatFs
 * takes its fast multi-sector disk_write() path. (f_expand() contiguous
 * preallocation would also help but FF_USE_EXPAND is disabled upstream.)
 */
#define SD_SECTOR_SZ   512u
#define WCOMBINE_SZ    (16u * SD_SECTOR_SZ)   /* 8 KB, multiple of sector */

static uint8_t wbuf[WCOMBINE_SZ] __aligned(4);
static size_t  wbuf_len;

/* Write the whole-sector portion of wbuf, carrying the sub-sector tail. Keeps
 * the file offset sector-aligned so the next fs_write stays on the fast path. */
static int wbuf_flush_sectors(struct storage_file *f)
{
	size_t aligned = wbuf_len & ~(SD_SECTOR_SZ - 1);

	if (aligned == 0) {
		return 0;
	}

	ssize_t w = fs_write(&f->fp, wbuf, aligned);

	if (w < 0) {
		return (int)w;
	}
	if ((size_t)w != aligned) {
		return -EIO;
	}

	size_t tail = wbuf_len - aligned;

	if (tail) {
		memmove(wbuf, wbuf + aligned, tail);
	}
	wbuf_len = tail;
	return 0;
}

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

/* Lowest-free logNNNN.tdms fallback (no wall clock, or file_fmt = index). */
static void name_indexed(char *path, size_t cap)
{
	struct fs_dirent ent;

	for (unsigned int idx = 1U; idx < 10000U; idx++) {
		snprintf(path, cap, "%s/log%04u.tdms", MOUNT_POINT, idx);
		if (fs_stat(path, &ent) != 0) {
			return;   /* first non-existent name -> use it */
		}
	}
}

static char s_last_path[80];

const char *storage_session_path(void)
{
	return s_last_path;
}

int storage_read_file(const char *path, void *buf, size_t cap, size_t *out_len)
{
	struct fs_file_t fp;
	fs_file_t_init(&fp);
	int rc = fs_open(&fp, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}
	ssize_t got = fs_read(&fp, buf, cap);
	(void)fs_close(&fp);
	if (got < 0) {
		return (int)got;
	}
	if (out_len) {
		*out_len = (size_t)got;
	}
	return 0;
}

int storage_write_file(const char *path, const void *buf, size_t len)
{
	struct fs_file_t fp;
	fs_file_t_init(&fp);
	int rc = fs_open(&fp, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc != 0) {
		return rc;
	}
	ssize_t w = fs_write(&fp, buf, len);
	int crc = fs_close(&fp);
	if (w < 0) return (int)w;
	if ((size_t)w != len) return -EIO;
	return crc;
}

int storage_statvfs(uint64_t *free_b, uint64_t *total_b)
{
	struct fs_statvfs st;
	int rc = fs_statvfs(MOUNT_POINT, &st);
	if (rc) {
		return rc;
	}
	uint64_t blksz = (uint64_t)st.f_frsize;
	if (free_b)  *free_b  = (uint64_t)st.f_bfree  * blksz;
	if (total_b) *total_b = (uint64_t)st.f_blocks * blksz;
	return 0;
}

int storage_file_size(const char *path, uint64_t *size_b)
{
	struct fs_dirent ent;
	int rc = fs_stat(path, &ent);
	if (rc) {
		return rc;
	}
	if (size_b) *size_b = (uint64_t)ent.size;
	return 0;
}

int storage_session_begin(struct storage_file *f)
{
	char path[80];
	struct fs_dirent ent;
	bool named = false;

	fs_file_t_init(&f->fp);

	/* A FRESH file each session (reusing a name without truncating leaves a
	 * longer prior session's stale tail, which corrupts TDMS readers). Prefer
	 * a UTC timestamp name (sortable == chronological; FAT-legal, colon-free);
	 * fall back to logNNNN when there's no wall clock or file_fmt = index. */
	if (config_get()->file_fmt == FILE_FMT_UTC && clk_has_time()) {
		time_t t = clk_now();
		struct tm tm;
		char base[24];

		gmtime_r(&t, &tm);
		snprintf(base, sizeof(base), "%04d%02d%02dT%02d%02d%02dZ",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			 tm.tm_hour, tm.tm_min, tm.tm_sec);
		snprintf(path, sizeof(path), "%s/%s.tdms", MOUNT_POINT, base);
		if (fs_stat(path, &ent) != 0) {
			named = true;
		} else {
			for (unsigned int k = 1; k < 100 && !named; k++) {
				snprintf(path, sizeof(path), "%s/%s_%02u.tdms",
					 MOUNT_POINT, base, k);
				named = (fs_stat(path, &ent) != 0);
			}
		}
	}
	if (!named) {
		name_indexed(path, sizeof(path));
	}

	/* TRUNC is belt-and-suspenders: the path picker above only returns names
	 * for which fs_stat said "not present", but a transient fs_stat miss (or a
	 * follow-up reopen of the same name within the same wall-clock second
	 * before the previous handle has flushed) would otherwise leave stale
	 * tail bytes that confuse npTDMS during segment walking. */
	int rc = fs_open(&f->fp, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);

	if (rc != 0) {
		LOG_ERR("fs_open(%s) failed: %d", path, rc);
		return rc;
	}
	wbuf_len = 0;
	strncpy(s_last_path, path, sizeof(s_last_path) - 1);
	s_last_path[sizeof(s_last_path) - 1] = '\0';
	LOG_INF("session file: %s", path);
	return 0;
}

int storage_write(struct storage_file *f, const void *buf, size_t len)
{
	const uint8_t *src = buf;

	while (len) {
		size_t space = WCOMBINE_SZ - wbuf_len;
		size_t n = len < space ? len : space;

		memcpy(wbuf + wbuf_len, src, n);
		wbuf_len += n;
		src += n;
		len -= n;

		if (wbuf_len == WCOMBINE_SZ) {
			ssize_t w = fs_write(&f->fp, wbuf, WCOMBINE_SZ);

			if (w < 0) {
				return (int)w;
			}
			if ((size_t)w != WCOMBINE_SZ) {
				return -EIO;
			}
			wbuf_len = 0;
		}
	}
	return 0;
}

int storage_sync(struct storage_file *f)
{
	/* Persist whole sectors only; the <512 B tail stays buffered so the file
	 * offset remains sector-aligned (small, recoverable loss on power-cut). */
	int rc = wbuf_flush_sectors(f);

	if (rc) {
		return rc;
	}
	return fs_sync(&f->fp);
}

size_t storage_wbuf_len(void)
{
	return wbuf_len;
}

int storage_session_end(struct storage_file *f)
{
	/* Final flush includes the partial last sector so the file is complete. */
	if (wbuf_len) {
		ssize_t w = fs_write(&f->fp, wbuf, wbuf_len);

		if (w < 0) {
			LOG_ERR("final flush failed: %d", (int)w);
		}
		wbuf_len = 0;
	}
	return fs_close(&f->fp);
}
