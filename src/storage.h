#ifndef STORAGE_H_
#define STORAGE_H_

#include <stddef.h>
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

#endif /* STORAGE_H_ */
