#include "logctl.h"
#include "logger.h"
#include "storage.h"
#include "config.h"
#include "clock.h"
#include "power.h"
#include "ble_stream.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_POWEROFF)
#include <zephyr/sys/poweroff.h>
#endif

LOG_MODULE_REGISTER(logctl, LOG_LEVEL_INF);

#define EVQ_LEN 8

K_MSGQ_DEFINE(evq, sizeof(uint8_t), EVQ_LEN, 1);

static enum logctl_state s_state = LS_STOPPED;
static bool s_usb = true;   /* VBUS present (seeded at init, tracked by events) */

static const char *const names[] = { "stopped", "logging", "offload", "ship", "idle" };

const char *logctl_state_name(enum logctl_state s) { return names[s]; }
enum logctl_state logctl_state(void) { return s_state; }

int logctl_post(enum logctl_event ev)
{
	uint8_t e = (uint8_t)ev;

	return k_msgq_put(&evq, &e, K_NO_WAIT);
}

/* ---- auto-off (dark-idle inactivity -> System OFF) --------------------- */
static void autooff_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(autooff_work, autooff_fn);

static void autooff_arm(void)
{
	const struct app_cfg *c = config_get();

	if (c->autooff_en && c->autooff_s > 0) {
		k_work_schedule(&autooff_work, K_SECONDS(c->autooff_s));
	}
}
static void autooff_cancel(void) { k_work_cancel_delayable(&autooff_work); }

static void autooff_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	logctl_post(EV_AUTOOFF);
}

/* ---- transitions ------------------------------------------------------- */
static void to_logging(void)
{
	if (s_state == LS_LOGGING) {
		return;
	}
	autooff_cancel();
	power_rail(true);                /* may be coming from dark idle */
	ble_stream_set_adv(true);
	logger_start();                  /* (re)mount + fresh session + acq */
	s_state = LS_LOGGING;
	LOG_INF("-> LOGGING");
}

static void enter_idle(void)        /* battery, not recording: go dark */
{
	(void)storage_unmount();         /* SD loses power with the rail */
	ble_stream_set_adv(config_get()->idle_ble);
	power_rail(false);               /* cut INA240/INA228/SD */
	autooff_arm();
	s_state = LS_IDLE;
	LOG_INF("-> IDLE (rail off, ble=%d, clock alive)", config_get()->idle_ble);
}

/* Stop recording -> the not-recording state appropriate for USB presence. */
static void stop_recording(void)
{
	if (s_state == LS_LOGGING) {
		logger_stop();           /* flush + close */
	}
	if (s_usb) {
		autooff_cancel();
		power_rail(true);
		ble_stream_set_adv(true);
		power_batt_sample();     /* SAADC free now */
		s_state = LS_STOPPED;
		LOG_INF("-> STOPPED");
	} else {
		power_batt_sample();     /* sample before the rail drops */
		enter_idle();
	}
}

static void wake_idle(void)         /* USB plugged (or REC) -> come back up */
{
	autooff_cancel();
	power_rail(true);
	ble_stream_set_adv(true);
	power_batt_sample();
	s_state = LS_STOPPED;
	LOG_INF("-> STOPPED (woke from idle)");
}

static void to_offload(void)
{
	if (s_state == LS_OFFLOAD) {
		return;
	}
	autooff_cancel();
	power_rail(true);                /* SD needs the rail (may be idle) */
	if (s_state == LS_LOGGING) {
		logger_stop();
	}
	(void)storage_unmount();         /* release FatFs so the host owns the blocks */
	power_batt_sample();
	s_state = LS_OFFLOAD;
	LOG_INF("-> OFFLOAD (SD released; host may mount the drive)");
}

static void to_ship(void)
{
	if (s_state == LS_LOGGING) {
		logger_stop();
	}
	clk_persist();
	power_rail(false);
	LOG_INF("-> SHIP (powering off)");
#if defined(CONFIG_POWEROFF)
	k_sleep(K_MSEC(50));             /* let logs/CDC drain */
	sys_poweroff();                  /* does not return; wakes via reset/GPIO */
#endif
	s_state = LS_SHIP;
}

static void handle(enum logctl_event ev)
{
	switch (ev) {
	case EV_REC_TOGGLE:
		(s_state == LS_LOGGING) ? stop_recording() : to_logging();
		break;
	case EV_LOG_START:
		to_logging();
		break;
	case EV_LOG_STOP:
		stop_recording();
		break;
	case EV_ROTATE:
		if (s_state == LS_LOGGING) {
			logger_rotate();
		}
		break;
	case EV_OFFLOAD_ON:
		to_offload();
		break;
	case EV_OFFLOAD_OFF:
		to_logging();            /* resume logging on offload exit */
		break;
	case EV_POWEROFF:
		to_ship();
		break;
	case EV_USB_ON:
		s_usb = true;
		if (s_state == LS_IDLE) {
			wake_idle();
		}
		break;
	case EV_USB_OFF:
		s_usb = false;
		/* battery now: if we're not recording, go dark. Logging continues. */
		if (s_state == LS_STOPPED || s_state == LS_OFFLOAD) {
			enter_idle();
		}
		break;
	case EV_AUTOOFF:
		if (s_state == LS_IDLE) {
			LOG_INF("idle inactivity -> ship");
			to_ship();
		}
		break;
	case EV_LOW_BATT:
		LOG_WRN("low battery");
		if (s_state == LS_LOGGING) {
			stop_recording();   /* flush + close; samples battery */
		}
		if (power_batt_mv() >= 0 &&
		    power_batt_mv() < (int)config_get()->pof_crit_mv) {
			LOG_WRN("battery critical (%d mV) -> ship", power_batt_mv());
			to_ship();
		}
		break;
	}
}

static void logctl_thread(void *a, void *b, void *c)
{
	uint8_t e;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (1) {
		if (k_msgq_get(&evq, &e, K_FOREVER) == 0) {
			handle((enum logctl_event)e);
		}
	}
}

/* Stack must cover logger_start()'s chain: FatFs mount + storage_session_begin
 * (gmtime_r/snprintf) + tdms_open. 2048 overflowed; match main's 4096. */
K_THREAD_DEFINE(logctl_tid, 4096, logctl_thread, NULL, NULL, NULL, 7, 0, 0);

void logctl_init(void)
{
	s_usb = power_usb_present();
	power_batt_sample();   /* SAADC free before logging starts */
	LOG_INF("logctl init: USB %s", s_usb ? "present" : "absent");
	if (config_get()->autostart) {
		logctl_post(EV_LOG_START);
	} else if (s_usb) {
		LOG_INF("autostart off -> STOPPED (use 'pwr log' / REC)");
	} else {
		logctl_post(EV_USB_OFF);   /* battery + no autostart -> dark idle */
	}
}
