#include "cal.h"
#include "config.h"

/* Offset in amps: fast_corrected = fast_raw - s_offset. EMA toward (fast-ina). */
static float s_offset;
static bool  s_primed;

void cal_reset(void)
{
	s_offset = 0.0f;
	s_primed = false;
}

float cal_offset_a(void)
{
	return config_get()->cal_enable ? s_offset : 0.0f;
}

void cal_update(float i_fast_raw_a, float i_ina_a, float dt_s, bool steady)
{
	float meas, tau, alpha;

	if (!config_get()->cal_enable || !steady || dt_s <= 0.0f) {
		return;
	}
	meas = i_fast_raw_a - i_ina_a;
	if (!s_primed) {            /* seed -> converge instantly on the first sample */
		s_offset = meas;
		s_primed = true;
		return;
	}
	tau = (float)config_get()->cal_tau_s;
	if (tau < 1.0f) {
		tau = 1.0f;
	}
	alpha = dt_s / tau;         /* slow loop: bandwidth well below the signal band */
	if (alpha > 1.0f) {
		alpha = 1.0f;
	}
	s_offset += alpha * (meas - s_offset);
}
