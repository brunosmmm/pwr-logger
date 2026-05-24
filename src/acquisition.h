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

/* Cumulative count of dropped buffers (pool exhausted / queue full). */
uint32_t acq_drops(void);

#endif /* ACQUISITION_H_ */
