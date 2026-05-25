#include "power.h"
#include "acquisition.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

/* External 3.3 V rail enable (was a permanent gpio-hog; now app-controlled so it
 * can be cut in dark idle / ship to drop the INA240 (~2 mA) + INA228 + SD). */
static const struct gpio_dt_spec rail =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rail_gpios);

void power_rail(bool on)
{
	if (gpio_is_ready_dt(&rail)) {
		gpio_pin_set_dt(&rail, on ? 1 : 0);
	}
}

int power_init(void)
{
	if (!gpio_is_ready_dt(&rail)) {
		LOG_ERR("rail GPIO not ready");
		return -ENODEV;
	}
	/* Drive ON immediately (before acq/ina228/storage come up). */
	gpio_pin_configure_dt(&rail, GPIO_OUTPUT_ACTIVE);
	LOG_INF("3V3 rail ON");
	return 0;
}

/* Last battery (VDDH) reading in mV; refreshed only when the SAADC is free. */
static int s_batt_mv = -1;

void power_batt_sample(void)
{
	int mv = acq_read_vddh_mv();

	if (mv >= 0) {
		s_batt_mv = mv;
	}
}

int power_batt_mv(void)
{
	return s_batt_mv;
}
