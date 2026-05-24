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
void agg_push(const int16_t *samples, uint16_t n);
bool agg_take(struct agg_window *out);  /* true if a window is ready */

#endif /* AGGREGATE_H_ */
