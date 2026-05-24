#include "ina228.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ina228, LOG_LEVEL_INF);

#define INA228_MSGQ_DEPTH 32
K_MSGQ_DEFINE(ina228_msgq, sizeof(struct ina228_sample), INA228_MSGQ_DEPTH, 4);

#define INA228_ADDR 0x40U

static const struct device *const i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct gpio_dt_spec cnvr =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), cnvr_gpios);

/*
 * TODO:
 *  - Configure CONFIG (reset, conv times, ADCRANGE) and ADC_CONFIG (continuous,
 *    averaging) for the precision/rate trade-off; program SHUNT_CAL.
 *  - Wire CNVR: GPIOTE event on the pin -> PPI -> TIMER.CAPTURE so each
 *    conversion-ready latches a master tick (DATA_FORMAT.md sect 1).
 *  - In the CNVR ISR/work: read VBUS, VSHUNT, POWER (24-bit) and DIETEMP, pair
 *    with the captured tick, k_msgq_put an ina228_sample.
 */

int ina228_init(void)
{
	if (!device_is_ready(i2c_bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&cnvr)) {
		LOG_ERR("CNVR GPIO not ready");
		return -ENODEV;
	}
	LOG_WRN("ina228_init: TODO configure + calibrate + CNVR capture (addr 0x%02x)",
		INA228_ADDR);
	return 0;
}

int ina228_start(void)
{
	LOG_INF("ina228_start (stub)");
	return 0;
}

int ina228_stop(void)
{
	LOG_INF("ina228_stop (stub)");
	return 0;
}
