#include "aggregate.h"

#include <math.h>
#include <limits.h>

/*
 * Single-producer/single-consumer: agg_push() and agg_take() are both called
 * from the logger writer thread, so no locking is needed. One window is held
 * "ready" at a time (overwritten if not taken before the next completes).
 */

#define AGG_WINDOW_SAMPLES 1000u   /* 50 ms windows at 20 kSPS -> ~20 Hz BLE live view */

static int64_t  s_sum;
static double   s_sumsq;
static int16_t  s_min;
static int16_t  s_max;
static uint32_t s_n;

static struct agg_window s_ready;
static bool s_have;

void agg_reset(void)
{
	s_sum = 0;
	s_sumsq = 0.0;
	s_min = INT16_MAX;
	s_max = INT16_MIN;
	s_n = 0;
	s_have = false;
}

void agg_push(const int16_t *samples, uint16_t n)
{
	for (uint16_t i = 0; i < n; i++) {
		int16_t v = samples[i];

		s_sum += v;
		s_sumsq += (double)v * (double)v;
		if (v < s_min) {
			s_min = v;
		}
		if (v > s_max) {
			s_max = v;
		}

		if (++s_n >= AGG_WINDOW_SAMPLES) {
			s_ready.n = s_n;
			s_ready.avg = (float)((double)s_sum / s_n);
			s_ready.rms = (float)sqrt(s_sumsq / s_n);
			s_ready.min = s_min;
			s_ready.max = s_max;
			s_ready.t_start = 0; /* TODO: window start tick from block */
			s_have = true;

			s_sum = 0;
			s_sumsq = 0.0;
			s_min = INT16_MAX;
			s_max = INT16_MIN;
			s_n = 0;
		}
	}
}

bool agg_take(struct agg_window *out)
{
	if (!s_have) {
		return false;
	}
	*out = s_ready;
	s_have = false;
	return true;
}
