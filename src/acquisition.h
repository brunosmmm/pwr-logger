#ifndef ACQUISITION_H_
#define ACQUISITION_H_

#include <stdint.h>
#include <zephyr/kernel.h>

/*
 * Fast current channel: INA240 -> SAADC @ 200 kSPS, nrfx + PPI + EasyDMA.
 * Filled DMA buffers are handed to the logger via acq_msgq (descriptor copied;
 * the underlying buffer is returned to the pool with acq_block_release()).
 */

struct acq_block {
	int16_t  *samples;    /* points into the buffer pool */
	uint16_t  count;      /* number of valid samples */
	uint32_t  t_capture;  /* raw 32-bit master tick latched at SAADC.END */
	uint32_t  seq;        /* per-channel sequence (gap detection) */
};

extern struct k_msgq acq_msgq;   /* of struct acq_block */

int  acq_init(void);
int  acq_start(void);
int  acq_stop(void);
void acq_block_release(struct acq_block *blk);

/* Count of dropped buffers (pool exhausted / queue full) since the last
 * acq_drops_reset(). logger_start() resets this at session begin so the
 * value displayed on FILE/SYS pages is per-session, not project-lifetime. */
uint32_t acq_drops(void);
void     acq_drops_reset(void);

/* One-shot battery read: SAADC VDDHDIV5 vs the internal 0.6 V ref, x5 -> VDDH mV.
 * ONLY valid while the fast channel is stopped (shares the single SAADC; the fast
 * path uses oversampling, which is single-channel only). Returns mV, <0 on error. */
int acq_read_vddh_mv(void);

#endif /* ACQUISITION_H_ */
