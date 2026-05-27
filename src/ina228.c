#include "ina228.h"
#include "app_config.h"
#include "timebase.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ina228, LOG_LEVEL_INF);

#define INA228_MSGQ_DEPTH 32
K_MSGQ_DEFINE(ina228_msgq, sizeof(struct ina228_sample), INA228_MSGQ_DEPTH, 4);

#define INA228_ADDR     0x40U
#define REG_CONFIG      0x00U
#define REG_ADC_CONFIG  0x01U
#define REG_SHUNT_CAL   0x02U
#define REG_VSHUNT      0x04U
#define REG_VBUS        0x05U
#define REG_DIETEMP     0x06U
#define REG_CURRENT     0x07U
#define REG_POWER       0x08U
#define REG_MANID       0x3EU

/* CONFIG: ADCRANGE=1 (±40.96 mV, high-res) for the small shunt drop. */
#define CFG_VAL         0x0010U
/* ADC_CONFIG: MODE=0xF (cont. bus+shunt+temp), VBUSCT/VSHCT/VTCT=5 (1052 us),
 * AVG=2 (16 averages) -> low-noise precision, ~20 SPS. */
#define ADC_CFG_VAL     0xFB6AU

/* Register LSBs */
#define VBUS_LSB_UV     195.3125f   /* µV/LSB (20-bit value) */
#define VSHUNT_LSB_NV   78.125f     /* nV/LSB, ADCRANGE=1 */
#define TEMP_LSB_mC     7.8125f     /* m°C/LSB */

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static float s_current_lsb;         /* A/LSB, from SHUNT_CAL */

static struct ina228_sample s_last;  /* latest sample, for cosmetic readers */
static volatile bool s_have_last;
static ina228_sample_cb_t s_sample_cb;

bool ina228_last(struct ina228_sample *out)
{
	if (!s_have_last) {
		return false;
	}
	*out = s_last;   /* benign torn-read risk; cosmetic only */
	return true;
}

void ina228_set_sample_cb(ina228_sample_cb_t cb)
{
	s_sample_cb = cb;
}

static struct k_thread rd_thread;
static k_tid_t rd_tid;
static volatile bool rd_running;
K_THREAD_STACK_DEFINE(rd_stack, 1536);

static int reg_write16(uint8_t reg, uint16_t val)
{
	uint8_t b[3] = { reg, (uint8_t)(val >> 8), (uint8_t)val };

	return i2c_write(i2c, b, sizeof(b), INA228_ADDR);
}

static int reg_read(uint8_t reg, uint8_t *data, size_t n)
{
	return i2c_write_read(i2c, INA228_ADDR, &reg, 1, data, n);
}

/* 24-bit register holding a 20-bit value in bits [23:4]. */
static int32_t read_20in24(uint8_t reg, bool is_signed)
{
	uint8_t b[3] = {0};
	int32_t v;

	(void)reg_read(reg, b, 3);
	v = ((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | b[2];
	v >>= 4;
	if (is_signed && (v & 0x80000)) {
		v |= ~0xFFFFF; /* sign-extend 20-bit */
	}
	return v;
}

/* 24-bit unsigned register (e.g. POWER) — uses the full 24 bits, no shift. */
static uint32_t read_24(uint8_t reg)
{
	uint8_t b[3] = {0};

	(void)reg_read(reg, b, 3);
	return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}

static void rd_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	int64_t t_log = 0;

	while (rd_running) {
		uint8_t tb[2] = {0};
		struct ina228_sample s = {
			.t_capture = timebase_now32(),
			.vbus_raw = read_20in24(REG_VBUS, false),
			.vshunt_raw = read_20in24(REG_VSHUNT, true),
			.current_raw = read_20in24(REG_CURRENT, true),
			.power_raw = (int32_t)read_24(REG_POWER),
		};

		(void)reg_read(REG_DIETEMP, tb, 2);
		s.temp_raw = (int16_t)((tb[0] << 8) | tb[1]);

		(void)k_msgq_put(&ina228_msgq, &s, K_NO_WAIT);

		/* Publish to the cosmetic-reader cache (display, shell `pwr
		 * status`). Plain assignment -- the read-side accepts torn
		 * reads ("benign torn-read risk; cosmetic only"). */
		s_last = s;
		s_have_last = true;

		/* Fire the stats/observer callback if attached. Runs in this
		 * thread -- keep callbacks short (stats.c is ~5 us). */
		if (s_sample_cb) {
			s_sample_cb(&s);
		}

		int64_t now = k_uptime_get();

		if (now - t_log >= 1000) {
			int vbus_uv = (int)(s.vbus_raw * VBUS_LSB_UV);          /* µV */
			int i_ua = (int)(s.current_raw * s_current_lsb * 1e6f); /* µA */
			/* INA228 POWER LSB = 3.2 * CURRENT_LSB */
			int p_uw = (int)(s.power_raw * 3.2f * s_current_lsb * 1e6f); /* µW */
			int t_mc = (int)(s.temp_raw * TEMP_LSB_mC);             /* m°C */

			LOG_INF("INA228: Vbus=%d uV  I=%d uA  P=%d uW  T=%d mC",
				vbus_uv, i_ua, p_uw, t_mc);
			t_log = now;
		}
		k_sleep(K_MSEC(50)); /* matches the ~20 SPS conversion cadence */
	}
}

int ina228_init(void)
{
	uint8_t id[2] = {0};
	uint16_t shunt_cal;

	if (!device_is_ready(i2c)) {
		LOG_ERR("I2C not ready");
		return -ENODEV;
	}

	(void)reg_write16(REG_CONFIG, 0x8000U); /* RST */
	k_msleep(2);
	(void)reg_write16(REG_CONFIG, CFG_VAL);
	(void)reg_write16(REG_ADC_CONFIG, ADC_CFG_VAL);

	/* SHUNT_CAL = 13107.2e6 * CURRENT_LSB * Rshunt, x4 for ADCRANGE=1. */
	s_current_lsb = INA228_IMAX_A / 524288.0f; /* 2^19 */
	shunt_cal = (uint16_t)(13107.2e6f * s_current_lsb * INA228_SHUNT_OHMS * 4.0f);
	(void)reg_write16(REG_SHUNT_CAL, shunt_cal);

	if (reg_read(REG_MANID, id, 2) == 0) {
		LOG_INF("ina228_init: MANID=0x%02x%02x, SHUNT_CAL=%u, CURRENT_LSB=%u nA",
			id[0], id[1], shunt_cal, (unsigned)(s_current_lsb * 1e9f));
	}
	return 0;
}

int ina228_start(void)
{
	rd_running = true;
	rd_tid = k_thread_create(&rd_thread, rd_stack, K_THREAD_STACK_SIZEOF(rd_stack),
				 rd_fn, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
	k_thread_name_set(rd_tid, "ina228");
	return 0;
}

int ina228_stop(void)
{
	rd_running = false;
	if (rd_tid != NULL) {
		k_thread_join(&rd_thread, K_FOREVER);
		rd_tid = NULL;
	}
	return 0;
}
