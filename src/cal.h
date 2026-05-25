#ifndef CAL_H_
#define CAL_H_

#include <stdbool.h>

/*
 * Fast-channel (INA240) zero auto-calibration against the INA228. Both shunts are
 * in series, so they carry the same current; the INA228 is the precision truth.
 * We track the slowly-drifting fast-channel offset as offset = LPF(i_fast - i_ina)
 * with a long time constant (config cal_tau_s), so the loop removes DC/thermal
 * drift without touching the fast channel's high-rate (AC/transient) content.
 */

void  cal_update(float i_fast_raw_a, float i_ina_a, float dt_s, bool steady);
float cal_offset_a(void);   /* subtract from the fast current to match the INA228 */
void  cal_reset(void);

#endif /* CAL_H_ */
