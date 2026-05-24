#include "acquisition.h"
#include "app_config.h"
#include "timebase.h"

#include <zephyr/kernel.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <nrfx.h>
#include <nrfx_saadc.h>

LOG_MODULE_REGISTER(acquisition, LOG_LEVEL_INF);

K_MSGQ_DEFINE(acq_msgq, sizeof(struct acq_block), ACQ_BUF_COUNT, 4);
K_MEM_SLAB_DEFINE(acq_slab, sizeof(int16_t) * ACQ_BUF_SAMPLES, ACQ_BUF_COUNT, 4);

/* Handed to the SAADC when the pool is exhausted; contents are discarded so
 * sampling never stalls (drop-on-overrun). */
static int16_t s_overflow[ACQ_BUF_SAMPLES];
static uint32_t s_seq;
static atomic_t s_drops;

/* SAADC internal sample timer: rate = 16 MHz / CC. 80 -> 200 kSPS. */
#define SAADC_TIMER_CC (16000000u / ACQ_SAMPLE_RATE_HZ)

static int16_t *pool_alloc(void)
{
	void *p;

	return (k_mem_slab_alloc(&acq_slab, &p, K_NO_WAIT) == 0) ? (int16_t *)p : NULL;
}

void acq_block_release(struct acq_block *blk)
{
	if (blk && blk->samples) {
		k_mem_slab_free(&acq_slab, (void *)blk->samples);
		blk->samples = NULL;
	}
}

uint32_t acq_drops(void)
{
	return (uint32_t)atomic_get(&s_drops);
}

/* nrfx SAADC event handler (ISR context). Keep it minimal: hand off buffers,
 * never block. */
static void saadc_evt(nrfx_saadc_evt_t const *evt)
{
	switch (evt->type) {
	case NRFX_SAADC_EVT_BUF_REQ: {
		int16_t *next = pool_alloc();

		if (next == NULL) {
			next = s_overflow; /* pool exhausted -> discard */
		}
		(void)nrfx_saadc_buffer_set(next, ACQ_BUF_SAMPLES);
		break;
	}
	case NRFX_SAADC_EVT_DONE: {
		int16_t *buf = evt->data.done.p_buffer;

		if (buf == s_overflow) {
			atomic_inc(&s_drops);
			break;
		}

		struct acq_block blk = {
			.samples = buf,
			.count = evt->data.done.size,
			.t_capture = timebase_now32(),
			.seq = s_seq++,
		};

		if (k_msgq_put(&acq_msgq, &blk, K_NO_WAIT) != 0) {
			k_mem_slab_free(&acq_slab, buf); /* consumer behind -> drop */
			atomic_inc(&s_drops);
		}
		break;
	}
	default:
		break;
	}
}

int acq_init(void)
{
	nrfx_saadc_channel_t ch =
		NRFX_SAADC_DEFAULT_CHANNEL_SE(NRFX_ANALOG_EXTERNAL_AIN7, 0);
	int err;

	ch.channel_config.gain = NRF_SAADC_GAIN1_6;
	ch.channel_config.reference = NRF_SAADC_REFERENCE_INTERNAL;
	ch.channel_config.acq_time = NRF_SAADC_ACQTIME_3US; /* required at 200 kSPS */

	IRQ_CONNECT(DT_IRQN(DT_NODELABEL(adc)), DT_IRQ(DT_NODELABEL(adc), priority),
		    nrfx_isr, nrfx_saadc_irq_handler, 0);

	err = nrfx_saadc_init(NRFX_SAADC_DEFAULT_CONFIG_IRQ_PRIORITY);
	if (err != 0 && err != -EALREADY) {
		LOG_ERR("nrfx_saadc_init -> %d", err);
		return -1;
	}
	err = nrfx_saadc_channel_config(&ch);
	if (err != 0) {
		LOG_ERR("nrfx_saadc_channel_config -> %d", err);
		return -1;
	}

	/* Calibrate the SAADC's internal offset (blocking; handler NULL). */
	(void)nrfx_saadc_offset_calibrate(NULL);

	LOG_INF("acq_init: AIN7, 12-bit, 3us acq, target %u kSPS",
		ACQ_SAMPLE_RATE_HZ / 1000u);
	return 0;
}

int acq_start(void)
{
	nrfx_saadc_adv_config_t adv = NRFX_SAADC_DEFAULT_ADV_CONFIG;
	int16_t *b0, *b1;
	int err;

	adv.internal_timer_cc = SAADC_TIMER_CC;
	adv.start_on_end = true; /* continuous: re-START on each END */

	err = nrfx_saadc_advanced_mode_set(BIT(0), NRF_SAADC_RESOLUTION_12BIT, &adv,
					   saadc_evt);
	if (err != 0) {
		LOG_ERR("advanced_mode_set -> %d", err);
		return -1;
	}

	b0 = pool_alloc();
	b1 = pool_alloc();
	if (b0 == NULL || b1 == NULL) {
		LOG_ERR("initial buffer alloc failed");
		return -1;
	}
	(void)nrfx_saadc_buffer_set(b0, ACQ_BUF_SAMPLES);
	(void)nrfx_saadc_buffer_set(b1, ACQ_BUF_SAMPLES);

	err = nrfx_saadc_mode_trigger();
	if (err != 0) {
		LOG_ERR("mode_trigger -> %d", err);
		return -1;
	}

	LOG_INF("acq_start: sampling (internal timer CC=%u)", SAADC_TIMER_CC);
	return 0;
}

int acq_stop(void)
{
	nrfx_saadc_abort();
	return 0;
}
