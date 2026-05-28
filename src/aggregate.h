#ifndef AGGREGATE_H_
#define AGGREGATE_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * Decimation / windowed statistics for the BLE live view. Fed from the fast
 * stream; produces compact summary windows. This is NOT the on-card format.
 */

struct agg_window {
	uint64_t t_start;   /* master tick of window start */
	float    rms;
	float    avg;
	int16_t  min;
	int16_t  max;
	uint32_t n;         /* samples in window */
};

void agg_reset(void);
/* Push `n` samples that arrived in the block whose first-sample tick is
 * `t_first_tick`. If the just-completed window had its first sample inside
 * this block, the aggregator stamps `t_start` for that window from
 * t_first_tick. (Pass 0 if you don't care -- t_start is still cosmetic.) */
void agg_push(const int16_t *samples, uint16_t n, uint64_t t_first_tick);
bool agg_take(struct agg_window *out);  /* true if a window is ready */

/* Optional observer fired the moment a new window is finalized (single
 * subscriber, runs in the producer thread -- keep the callback short).
 * Pass NULL to detach. Used by stats.c for the rolling/HOLD engine. */
typedef void (*agg_window_cb_t)(const struct agg_window *);
void agg_set_window_cb(agg_window_cb_t cb);

#endif /* AGGREGATE_H_ */
