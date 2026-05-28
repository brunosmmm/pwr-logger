#ifndef STORAGE_H_
#define STORAGE_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/fs/fs.h>

/*
 * Storage seam (REQUIREMENTS.md section 4). The default backend is exFAT via
 * FatFs; the TDMS encoder writes opaque aligned blocks through this interface,
 * so a raw-sector backend could be swapped in without touching tdms.c/logger.c.
 */

struct storage_file {
	struct fs_file_t fp;
	/* TODO: f_expand contiguous pre-allocation + valid-length tracking */
};

int storage_mount(void);
int storage_unmount(void);

int storage_session_begin(struct storage_file *f); /* opens a fresh logNNNN.tdms */
int storage_write(struct storage_file *f, const void *buf, size_t len);
int storage_sync(struct storage_file *f);
int storage_session_end(struct storage_file *f);

/* Last session path opened (or "" if none yet). Static buffer; do not free. */
const char *storage_session_path(void);

/* Read up to `cap` bytes from `path` into `buf`. Sets *out_len to bytes read.
 * Returns 0 on success (including 0-byte files); negative errno on failure.
 * Caller must have called storage_mount() first (no auto-mount). For small
 * text files like /SD:/pwr.cfg. */
int storage_read_file(const char *path, void *buf, size_t cap, size_t *out_len);

/* Open `path` for write, truncating any existing content. Closes immediately
 * after writing all `len` bytes. Returns 0 / negative errno. */
int storage_write_file(const char *path, const void *buf, size_t len);

/* Free and total bytes on the mounted SD. Returns 0 on success; <0 if not
 * mounted or fs_statvfs fails. Cheap (a few hundred us). */
int storage_statvfs(uint64_t *free_b, uint64_t *total_b);

/* Size of an arbitrary file in bytes via fs_stat. Returns 0 / -errno. */
int storage_file_size(const char *path, uint64_t *size_b);

#endif /* STORAGE_H_ */
