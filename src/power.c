#include "power.h"
#include "acquisition.h"
#include "config.h"
#include "logctl.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

/* External 3.3 V rail enable (was a permanent gpio-hog; now app-controlled so it
 * can be cut in dark idle / ship to drop the INA240 (~2 mA) + INA228 + SD). */
static const struct gpio_dt_spec rail =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), rail_gpios);

static void pofcon_arm(uint16_t trip_mv);

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

	pofcon_arm(config_get()->pof_trip_mv);
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

/* ---- POFCON: low-battery (VDDH) clean-shutdown ------------------------- */
/* Largest VDDH threshold <= the requested mV (2.7-4.2 V in 0.1 V steps). */
static nrf_power_pof_thrvddh_t vddh_thr(uint16_t mv)
{
	static const struct {
		uint16_t mv;
		nrf_power_pof_thrvddh_t t;
	} tbl[] = {
		{4200, NRF_POWER_POFTHRVDDH_V42}, {4100, NRF_POWER_POFTHRVDDH_V41},
		{4000, NRF_POWER_POFTHRVDDH_V40}, {3900, NRF_POWER_POFTHRVDDH_V39},
		{3800, NRF_POWER_POFTHRVDDH_V38}, {3700, NRF_POWER_POFTHRVDDH_V37},
		{3600, NRF_POWER_POFTHRVDDH_V36}, {3500, NRF_POWER_POFTHRVDDH_V35},
		{3400, NRF_POWER_POFTHRVDDH_V34}, {3300, NRF_POWER_POFTHRVDDH_V33},
		{3200, NRF_POWER_POFTHRVDDH_V32}, {3100, NRF_POWER_POFTHRVDDH_V31},
		{3000, NRF_POWER_POFTHRVDDH_V30}, {2900, NRF_POWER_POFTHRVDDH_V29},
		{2800, NRF_POWER_POFTHRVDDH_V28}, {2700, NRF_POWER_POFTHRVDDH_V27},
	};

	for (size_t i = 0; i < ARRAY_SIZE(tbl); i++) {
		if (mv >= tbl[i].mv) {
			return tbl[i].t;
		}
	}
	return NRF_POWER_POFTHRVDDH_V27;
}

static bool s_pof_tripped;

/* Poll POFWARN (avoids the POWER/CLOCK shared IRQ). Hold-up is seconds, so 200 ms
 * is ample to flush + close the TDMS file before brown-out. */
static void pof_poll_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(pof_poll, pof_poll_fn);

static void pof_poll_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	if (nrf_power_event_check(NRF_POWER, NRF_POWER_EVENT_POFWARN)) {
		nrf_power_event_clear(NRF_POWER, NRF_POWER_EVENT_POFWARN);
		if (!s_pof_tripped) {
			s_pof_tripped = true;
			LOG_WRN("POFCON: VDDH below trip -> low-battery shutdown");
			logctl_post(EV_LOW_BATT);
		}
	}
	k_work_schedule(&pof_poll, K_MSEC(200));
}

static void pofcon_arm(uint16_t trip_mv)
{
	nrf_power_mainregstatus_t reg;

	nrf_power_pofcon_set(NRF_POWER, true, NRF_POWER_POFTHR_V28); /* enable; VDD thr unused in HV */
	nrf_power_pofcon_vddh_set(NRF_POWER, vddh_thr(trip_mv));
	nrf_power_event_clear(NRF_POWER, NRF_POWER_EVENT_POFWARN);
	reg = nrf_power_mainregstatus_get(NRF_POWER);
	LOG_INF("POFCON armed: VDDH trip ~%u mV, supply=%s", trip_mv,
		reg == NRF_POWER_MAINREGSTATUS_HIGH ? "HV(VDDH)" : "normal(VDD)");
	k_work_schedule(&pof_poll, K_MSEC(500));
}
