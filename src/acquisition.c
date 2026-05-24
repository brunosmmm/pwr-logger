#include "acquisition.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(acquisition, LOG_LEVEL_INF);

K_MSGQ_DEFINE(acq_msgq, sizeof(struct acq_block), ACQ_BUF_COUNT, 4);

/* DMA buffer pool: ACQ_BUF_COUNT buffers of ACQ_BUF_SAMPLES int16 each. */
K_MEM_SLAB_DEFINE(acq_slab, sizeof(int16_t) * ACQ_BUF_SAMPLES, ACQ_BUF_COUNT, 4);

/*
 * TODO: nrfx_saadc, single channel, 12-bit, ACQTIME 3 us, gain/ref mapping the
 * INA240 0..3.3 V output (internal 0.6 V ref + gain 1/6 -> 3.6 V FS).
 * nrfx_timer @ ACQ_SAMPLE_RATE_HZ; PPI:
 *     TIMER.COMPARE -> SAADC.SAMPLE   (trigger, no CPU)
 *     SAADC.END     -> TIMER.CAPTURE  (timestamp, no CPU; via timebase)
 * Gapless double-buffer:
 *     NRFX_SAADC_EVT_BUF_REQ -> k_mem_slab_alloc + nrfx_saadc_buffer_set(next)
 *     NRFX_SAADC_EVT_DONE    -> fill acq_block (samples, count, captured tick,
 *                               seq++) and k_msgq_put. On full queue: drop +
 *                               emit a gap event (see logger/tdms).
 */

int acq_init(void)
{
	LOG_WRN("acq_init: TODO wire nrfx SAADC + TIMER + PPI double-buffering");
	return 0;
}

int acq_start(void)
{
	LOG_INF("acq_start (stub)");
	return 0;
}

int acq_stop(void)
{
	LOG_INF("acq_stop (stub)");
	return 0;
}

void acq_block_release(struct acq_block *blk)
{
	if (blk && blk->samples) {
		k_mem_slab_free(&acq_slab, (void *)blk->samples);
		blk->samples = NULL;
	}
}
