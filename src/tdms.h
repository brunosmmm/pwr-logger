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
	bool     fast_meta;        /* first FULL fast-metadata segment emitted     */
	bool     slow_meta;        /* first FULL slow-metadata segment emitted     */
	uint8_t  last_objset;      /* 0=none 1=fast 2=slow: prev seg's raw obj set */
	uint32_t fast_seq;

	/* Session wall-clock anchor, set at the first fast write. Absolute NI
	 * timestamps = base_unix + (tick - base_tick)/master_clock. */
	bool     have_anchor;
	bool     abs_ok;           /* anchor carries real wall time, not relative  */
	int64_t  base_unix;        /* wall seconds at base_tick                     */
	uint64_t base_tick;        /* 64-bit master tick at session start          */
	uint32_t slow_prev32;      /* slow 32-bit tick extender state              */
	uint64_t slow_hi;

	uint32_t marker_seq;       /* 1, 2, 3, ... -- key suffix for "marker_N" props */
};

int tdms_open(struct tdms_writer *w, struct storage_file *f);
int tdms_write_fast(struct tdms_writer *w, const int16_t *samples,
		    uint16_t n, uint64_t t_first_tick);
int tdms_write_slow(struct tdms_writer *w, const struct ina228_sample *s,
		    size_t n);
int tdms_write_event(struct tdms_writer *w, uint64_t tick, const char *msg);
int tdms_close(struct tdms_writer *w);

#endif /* TDMS_H_ */
