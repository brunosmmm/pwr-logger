#include "timebase.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(timebase, LOG_LEVEL_INF);

/*
 * TODO: configure an nrfx_timer (e.g. TIMER2) in 32-bit mode at 1 MHz,
 * free-running. Reserve CC channels for PPI hardware-capture:
 *   - SAADC.END           -> TIMER.CAPTURE[n]  (fast buffer timestamp)
 *   - INA228 CNVR/GPIOTE  -> TIMER.CAPTURE[m]  (slow sample timestamp)
 * timebase_now32() reads a spare CC via nrfx_timer_capture().
 */

static uint32_t s_last;   /* last captured low word (wrap detection) */
static uint32_t s_high;   /* high word, bumped on wrap */

int timebase_init(void)
{
	s_last = 0;
	s_high = 0;
	LOG_WRN("timebase_init: TODO start 1 MHz free-running TIMER + PPI capture");
	return 0;
}

uint32_t timebase_now32(void)
{
	/* TODO: return nrfx_timer_capture(&master_timer, NRF_TIMER_CC_CHANNEL3); */
	return 0;
}

uint64_t timebase_extend(uint32_t t32)
{
	if (t32 < s_last) {
		s_high++;   /* wrap: called far faster than the ~71 min period */
	}
	s_last = t32;
	return ((uint64_t)s_high << 32) | t32;
}
