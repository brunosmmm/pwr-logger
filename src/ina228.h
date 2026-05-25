#ifndef INA228_H_
#define INA228_H_

#include <stdint.h>
#include <zephyr/kernel.h>

/*
 * Precision channel: INA228 over I2C. Each conversion-ready (CNVR) pulse is
 * hardware-timestamped (GPIOTE -> PPI -> TIMER.CAPTURE); the ISR reads the
 * registers and posts a sample. Raw register values are kept; scaling happens
 * via TDMS linear-scale properties (DATA_FORMAT.md sections 4 & 5).
 */

struct ina228_sample {
	uint32_t t_capture;  /* raw 32-bit master tick at CNVR */
	int32_t  vbus_raw;   /* 24-bit, sign-extended */
	int32_t  vshunt_raw; /* 24-bit, sign-extended */
	int32_t  current_raw;/* CURRENT register, 24-bit sign-extended */
	int32_t  power_raw;  /* POWER register, 24-bit unsigned */
	int16_t  temp_raw;   /* DIETEMP */
};

extern struct k_msgq ina228_msgq;  /* of struct ina228_sample */

int ina228_init(void);
int ina228_start(void);
int ina228_stop(void);

/* Latest sample (cached by the read thread) for cosmetic readers (display).
 * Returns false if none yet. Not for the data path (use ina228_msgq). */
bool ina228_last(struct ina228_sample *out);

#endif /* INA228_H_ */
