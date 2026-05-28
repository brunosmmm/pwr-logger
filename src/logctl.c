#include "logctl.h"
#include "logger.h"
#include "storage.h"
#include "config.h"
#include "clock.h"
#include "power.h"
#include "ble_stream.h"
#include "display.h"

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

/* ---- auto-off (no longer arms System OFF) ----------------------------- *
 * Historically this scheduled an EV_AUTOOFF after `autooff_s` of dark idle
 * which then called to_ship() -> sys_poweroff(). We never want that: System
 * OFF resets the RTC peripherals and wipes uptime, so the LFXO-backed wall
 * clock has to re-sync from NVS on every wake -- and System OFF on this
 * board has no GPIO-SENSE / VBUSDETECT wake configured, so a press on REC
 * or a USB plug-in is silently ignored until somebody hits the reset pin.
 *
 * The agreed design is "System ON deep sleep is the only sleep state."
 * sys_poweroff() runs ONLY on the explicit user EV_POWEROFF (CLI `pwr
 * poweroff` / BLE PWR_OP_POWEROFF). Dark-idle stays in System ON forever;
 * the LFXO keeps RTC1 ticking, USBD_MSG_VBUS_READY wakes us instantly,
 * and the existing GPIO EDGE_BOTH interrupts on REC/ROTATE post their
 * events into logctl as soon as the user touches a button.
 *
 * Kept as no-ops so existing callers / config keys (`autooff_en`,
 * `autooff_s`) stay valid; reintroduce a real action here only if we add
 * a deeper-tier sleep that does NOT call sys_poweroff(). */
static void autooff_arm(void)   { /* no-op: see header comment */ }
static void autooff_cancel(void) { /* no-op: see header comment */ }

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

/* Two-phase IDLE entry: phase 1 shows a notice on the display + schedules a
 * delayed finalize so the operator gets a visible heads-up before the rail
 * cuts. Phase 2 (`idle_finalize_fn`) does the actual unmount + rail-off.
 * EV_USB_ON during the warning window aborts the pending shutdown. */
#define IDLE_WARN_MS 3000

static void idle_finalize_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(idle_finalize_work, idle_finalize_fn);

static void enter_idle_finalize(void)
{
	(void)storage_unmount();         /* SD loses power with the rail */
	ble_stream_set_adv(config_get()->idle_ble);
	display_set_powered(false);      /* render thread stops before rail cuts */
	power_rail(false);               /* cut INA240/INA228/SD/display */
	autooff_arm();
	s_state = LS_IDLE;
	LOG_INF("-> IDLE (rail off, ble=%d, clock alive)", config_get()->idle_ble);
}

static void idle_finalize_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	/* All other transitions cancel this work via cancel_pending_idle();
	 * by the time the timer fires, the only thing that could legitimately
	 * abort is USB coming back -- and that path also cancels us, so this
	 * is just a belt-and-braces guard. Specifically do NOT gate on s_state
	 * being LS_LOGGING: stop_recording() -> enter_idle() leaves s_state as
	 * LS_LOGGING through the warning window (we don't transition until
	 * finalize), and gating on it here would silently skip the rail cut. */
	if (s_usb) {
		display_notice(NULL, 0);
		return;
	}
	enter_idle_finalize();
}

static void enter_idle(void)        /* battery, not recording: go dark */
{
	if (IDLE_WARN_MS > 0) {
		display_notice("powering off", IDLE_WARN_MS);
		LOG_INF("-> IDLE in %d ms", IDLE_WARN_MS);
		k_work_schedule(&idle_finalize_work, K_MSEC(IDLE_WARN_MS));
	} else {
		enter_idle_finalize();
	}
}

static void cancel_pending_idle(void)
{
	k_work_cancel_delayable(&idle_finalize_work);
	display_notice(NULL, 0);
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
	display_set_powered(true);       /* SSD1306 re-init: rail just came back */
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
		cancel_pending_idle();
		if (s_state == LS_OFFLOAD) {
			/* OFFLOAD is a maintained-switch state; a momentary
			 * button must not override it (and starting LOGGING
			 * would race the MSC host for the SD). User must flip
			 * the switch out of OFFLOAD first. */
			LOG_INF("REC ignored: OFFLOAD switch engaged");
			display_notice("offload locked", 1500);
			break;
		}
		(s_state == LS_LOGGING) ? stop_recording() : to_logging();
		break;
	case EV_LOG_START:
		cancel_pending_idle();
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
		cancel_pending_idle();
		to_offload();
		break;
	case EV_OFFLOAD_OFF:
		to_logging();            /* resume logging on offload exit */
		break;
	case EV_POWEROFF:
		cancel_pending_idle();
		to_ship();
		break;
	case EV_USB_ON:
		s_usb = true;
		cancel_pending_idle();   /* aborted shutdown if we were in the warn window */
		if (s_state == LS_IDLE) {
			wake_idle();         /* full re-wake if we already finalized */
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
		/* See autooff header comment: never call to_ship() from here.
		 * Dark idle stays in System ON deep sleep indefinitely. */
		break;
	case EV_LOW_BATT:
		LOG_WRN("low battery (%d mV)", power_batt_mv());
		if (s_state == LS_LOGGING) {
			stop_recording();   /* flush + close so the file is safe */
		}
		/* Do NOT to_ship() even when batt is below pof_crit_mv. The
		 * hardware POFCON brown-out + RAM-retention reset are the
		 * physical safety net; calling sys_poweroff() here would also
		 * reset the LFXO-backed clock and leave nothing for the user
		 * to press to come back. Stay in dark idle; if VDDH genuinely
		 * collapses, the chip browns out on its own. */
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
