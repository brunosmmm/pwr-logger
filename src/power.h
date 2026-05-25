#ifndef POWER_H_
#define POWER_H_

#include <stdbool.h>

/*
 * Power management: the external 3.3 V rail gate (powers INA228/INA240/SD), DC/DC,
 * battery monitoring (POFCON trip + sweep gauge, SAADC VDDHDIV5 readout) and the
 * USB-VBUS gate. Built up across Step 4 sub-commits.
 */

int  power_init(void);       /* rail ON; (POFCON / DC/DC added in later sub-steps) */
void power_rail(bool on);    /* enable/disable the front-end 3.3 V rail */

int  power_batt_mv(void);    /* last cached VDDH (battery) reading, mV; <0 = unknown */
void power_batt_sample(void); /* refresh the reading; call ONLY when the SAADC is free
			      * (fast channel stopped) -- e.g. boot, idle, offload */

bool power_usb_present(void); /* VBUS detected right now (USB plugged) */

#endif /* POWER_H_ */
