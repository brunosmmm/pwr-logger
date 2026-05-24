#ifndef TDMS_H_
#define TDMS_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "ina228.h"

/*
 * NI TDMS encoder. One segment per DMA buffer / batched slow flush. Writes
 * through the storage seam. See DATA_FORMAT.md sections 2-6 for the segment /
 * object / property layout and the byte-level details to verify vs. the NI spec.
 */

struct storage_file; /* fwd decl (storage.h) */

struct tdms_writer {
	struct storage_file *file;
	bool     header_written;   /* first segment carries metadata + obj list */
	uint8_t  last_objset;      /* 0 = none, 1 = fast, 2 = slow (for ToC reuse) */
	uint32_t fast_seq;
};

int tdms_open(struct tdms_writer *w, struct storage_file *f);
int tdms_write_fast(struct tdms_writer *w, const int16_t *samples,
		    uint16_t n, uint64_t t_first_tick);
int tdms_write_slow(struct tdms_writer *w, const struct ina228_sample *s,
		    size_t n);
int tdms_write_event(struct tdms_writer *w, uint64_t tick, const char *msg);
int tdms_close(struct tdms_writer *w);

#endif /* TDMS_H_ */
