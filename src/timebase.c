#include "timebase.h"
#include "app_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_timer.h>

LOG_MODULE_REGISTER(timebase, LOG_LEVEL_INF);

/*
 * Master time base: TIMER2 free-running, 32-bit, 1 MHz (16 MHz / 2^4). Read by
 * capturing into a spare CC. (PPI capture on SAADC.END / INA228 CNVR will be
 * wired to other CC channels in a later increment.)
 */
#define MASTER_TIMER NRF_TIMER2
/* nRF52840 TIMER2 has CC0..CC3 only (4 channels). CC4/CC5 exist on TIMER3/4
 * but not here -- accessing them silently returns 0, which made every
 * t_capture / wf_start_time / slow timestamp read as zero. Use CC3 and leave
 * CC0..CC2 free for future PPI captures (SAADC.END, INA228 CNVR). */
#define READ_CC      NRF_TIMER_CC_CHANNEL3

static uint32_t s_last; /* last captured low word (for 32->64 extension) */
static uint32_t s_high; /* high word, bumped on wrap */

int timebase_init(void)
{
	s_last = 0;
	s_high = 0;

	nrf_timer_task_trigger(MASTER_TIMER, NRF_TIMER_TASK_STOP);
	nrf_timer_mode_set(MASTER_TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(MASTER_TIMER, NRF_TIMER_BIT_WIDTH_32);
	nrf_timer_prescaler_set(MASTER_TIMER, 4); /* 16 MHz / 2^4 = 1 MHz */
	nrf_timer_task_trigger(MASTER_TIMER, NRF_TIMER_TASK_CLEAR);

	LOG_INF("timebase: TIMER2 configured @ %u Hz (stopped)", MASTER_CLOCK_HZ);
	return 0;
}

void timebase_start(void)
{
	/* Acquisition starting -- reset wrap state to match the fresh
	 * counter run. The old s_high counted wraps since boot; if we
	 * stopped/restarted the timer the counter restarts from 0 so we
	 * need a fresh wrap baseline. */
	s_last = 0;
	s_high = 0;
	nrf_timer_task_trigger(MASTER_TIMER, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(MASTER_TIMER, NRF_TIMER_TASK_START);
}

void timebase_stop(void)
{
	/* Acquisition stopping -- release HFCLK demand. */
	nrf_timer_task_trigger(MASTER_TIMER, NRF_TIMER_TASK_STOP);
}

uint32_t timebase_now32(void)
{
	nrf_timer_task_trigger(MASTER_TIMER, nrf_timer_capture_task_get(READ_CC));
	return nrf_timer_cc_get(MASTER_TIMER, READ_CC);
}

uint64_t timebase_extend(uint32_t t32)
{
	if (t32 < s_last) {
		s_high++; /* wrap (called far faster than the ~71 min period) */
	}
	s_last = t32;
	return ((uint64_t)s_high << 32) | t32;
}
