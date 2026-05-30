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
		LOG_INF("rail -> %s (pin P0.%u)", on ? "ON" : "OFF", rail.pin);
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

void power_batt_invalidate(void)
{
	s_batt_mv = -1;
}

/* Refresh s_batt_mv while the fast-channel acquisition is running. The SAADC
 * is a single-core peripheral; the continuous AIN7 mode has to be paused for
 * the VDDHDIV5 one-shot. Cost: one ~50 ms drop in the fast stream per call,
 * for the abort/settle/sample/restart cycle. Intended for one-shot events like
 * USB plug/unplug, NOT a periodic poll. Safe to call when acq is not running
 * (acq_stop is a no-op then; acq_start re-arms the same continuous config). */
void power_batt_refresh_during_acq(void)
{
	acq_stop();
	k_sleep(K_MSEC(5));        /* let in-flight DMA + BUF_REQ drain */
	power_batt_sample();
	acq_start();
}

int power_batt_mv(void)
{
	return s_batt_mv;
}

/* Single-cell Li-ion state-of-charge from terminal voltage. Piecewise
 * linear over the canonical 5-point OCV table reproduced across most
 * generic Li-ion references (ufinebattery.com, jackery.com, etc.):
 *
 *   100 % = 4.20 V    75 % = 3.90 V    50 % = 3.70 V
 *    25 % = 3.50 V     0 % = 3.00 V
 *
 * Honest about what this is and isn't:
 *
 *   - Battery University BU-903 calls voltage-only SoC "impracticable"
 *     for Li-ion: 80 % of the cell's energy lives in the flat plateau
 *     around 3.7 V where voltage barely changes with charge. We can
 *     reliably tell "near full" (>4.0 V) and "near empty" (<3.4 V); the
 *     middle reads are accurate to roughly +/- 10-15 % SoC.
 *   - Reads terminal voltage, not open-circuit. Under load the cell
 *     sags ~50-100 mV so SoC under-reports during current bursts.
 *   - Generic curve; not measured against the specific cell. Cell
 *     aging shifts it further.
 *
 * Good enough for a "is the battery roughly full / half / dead?"
 * indicator. For real fuel-gauging use coulomb counting + an OCV
 * re-anchor, or a dedicated IC (MAX17048 / BQ27441). */
int power_batt_pct(void)
{
	static const struct { int mv; uint8_t pct; } curve[] = {
		{ 4200, 100 }, { 3900, 75 }, { 3700, 50 }, { 3500, 25 }, { 3000, 0 },
	};
	int mv = s_batt_mv;

	if (mv < 0) {
		return -1;
	}
	if (mv >= curve[0].mv) {
		return 100;
	}
	if (mv <= curve[ARRAY_SIZE(curve) - 1].mv) {
		return 0;
	}
	for (size_t i = 0; i < ARRAY_SIZE(curve) - 1; i++) {
		if (mv <= curve[i].mv && mv > curve[i + 1].mv) {
			int dmv = curve[i].mv - curve[i + 1].mv;
			int dpct = (int)curve[i].pct - (int)curve[i + 1].pct;
			int frac = (mv - curve[i + 1].mv) * dpct / dmv;
			return (int)curve[i + 1].pct + frac;
		}
	}
	return -1;
}

bool power_usb_present(void)
{
	return nrf_power_usbregstatus_vbusdet_get(NRF_POWER);
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

void power_pause(void)
{
	k_work_cancel_delayable(&pof_poll);
}

void power_resume(void)
{
	k_work_schedule(&pof_poll, K_MSEC(500));
}

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

	/* VDD threshold: set to the LOWEST option (V17 = 1.7 V) so it never
	 * trips spuriously when REG0 is at its default 1.8 V output. The V28
	 * default sat ABOVE our VDD, which kept POFWARN permanently latched,
	 * which made every NVS/flash write fail with -ECANCELED via the
	 * nRF52 erratum-242 path in soc_flash_nrf. Real low-battery detection
	 * happens via the VDDH threshold below (HV mode). */
	nrf_power_pofcon_set(NRF_POWER, true, NRF_POWER_POFTHR_V17);
	nrf_power_pofcon_vddh_set(NRF_POWER, vddh_thr(trip_mv));
	nrf_power_event_clear(NRF_POWER, NRF_POWER_EVENT_POFWARN);
	reg = nrf_power_mainregstatus_get(NRF_POWER);
	LOG_INF("POFCON armed: VDDH trip ~%u mV, supply=%s", trip_mv,
		reg == NRF_POWER_MAINREGSTATUS_HIGH ? "HV(VDDH)" : "normal(VDD)");
	k_work_schedule(&pof_poll, K_MSEC(500));
}
