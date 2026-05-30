#include "logctl.h"
#include "logger.h"
#include "storage.h"
#include "config.h"
#include "clock.h"
#include "power.h"
#include "ble_stream.h"
#include "display.h"
#include "ota.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/pm/device.h>
#if defined(CONFIG_POWEROFF)
#include <zephyr/sys/poweroff.h>
#endif

LOG_MODULE_REGISTER(logctl, LOG_LEVEL_INF);

/* Switched-rail peripheral bus devices. Suspended before the rail drops
 * so the bus drivers apply pinctrl-sleep (low-power-enable disconnects
 * the pads). Otherwise the MCU keeps driving SDA/SCL/SCK/MOSI/CS into
 * the now-unpowered slaves and current leaks through their ESD diodes
 * -- the LS_IDLE rail-off was costing mA instead of microamps. */
static const struct device *const i2c0_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const i2c1_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));
static const struct device *const spi2_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));

/* INA228 CNVR open-drain input has an internal pull-up to MCU VDD.
 * When INA228 loses power, that pull-up keeps sourcing current through
 * the chip's ESD clamp. Reconfigure as input-no-pull before rail-off,
 * restore with pull-up on wake. P1.6 per the board overlay. */
static const struct gpio_dt_spec cnvr = {
	.port = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
	.pin = 6,
	.dt_flags = GPIO_ACTIVE_LOW | GPIO_PULL_UP,
};

static void switched_rail_suspend(void)
{
	(void)pm_device_action_run(i2c0_dev, PM_DEVICE_ACTION_SUSPEND);
	(void)pm_device_action_run(i2c1_dev, PM_DEVICE_ACTION_SUSPEND);
	(void)pm_device_action_run(spi2_dev, PM_DEVICE_ACTION_SUSPEND);
	(void)gpio_pin_configure_dt(&cnvr, GPIO_INPUT);   /* no pull */
}

static void switched_rail_resume(void)
{
	(void)gpio_pin_configure_dt(&cnvr, GPIO_INPUT | GPIO_PULL_UP);
	(void)pm_device_action_run(spi2_dev, PM_DEVICE_ACTION_RESUME);
	(void)pm_device_action_run(i2c1_dev, PM_DEVICE_ACTION_RESUME);
	(void)pm_device_action_run(i2c0_dev, PM_DEVICE_ACTION_RESUME);
}

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

/* ---- auto-sleep: STOPPED-on-battery -> IDLE after autooff_s of inactivity *
 * Repurposed from the original "dark-idle -> System OFF" behavior (commit
 * b5787a4) which we excised because System OFF resets the LFXO-backed clock
 * and has no configured wake source. Now the timer just transitions
 * LS_STOPPED -> LS_IDLE via the normal enter_idle() path (3 s 'sleeping'
 * banner -> rail off -> stays in System ON with the RTC ticking + button +
 * USB wake sources intact). The user-facing config keys stay the same
 * (autooff_en, autooff_s); only the destination changes.
 *
 * Armed by every transition that lands in LS_STOPPED on battery; cancelled
 * by any user action that implies "I am here doing something" (button press
 * -> LOGGING / OFFLOAD / EV_WAKE, USB plug-in -> wake_idle / STOPPED-on-USB,
 * etc.) -- the existing autooff_cancel call sites cover all of those. */
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

void logctl_keepalive(void)
{
	/* Only meaningful in states where the autooff timer can actually
	 * be armed (battery-side STOPPED/IDLE). Cheap no-op otherwise so
	 * callers don't need to gate their own pings. Cancel + re-arm
	 * gives a full fresh window from "now". */
	if (logctl_state() == LS_STOPPED || logctl_state() == LS_IDLE) {
		autooff_cancel();
		autooff_arm();
	}
}

static void autooff_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	/* Inhibit auto-sleep while a BLE OTA upload is in flight. The
	 * device IS doing useful work (BLE transport is alive, mcumgr
	 * is writing flash) -- dropping the rail mid-upload would
	 * suspend the display + SD but BLE keeps running, so the user
	 * would see a silent "screen off but upload continuing"
	 * confusion. Re-arm and try again later. */
	if (ota_in_progress()) {
		autooff_arm();
		return;
	}
	logctl_post(EV_AUTOOFF);
}

/* ---- transitions ------------------------------------------------------- */
static void cancel_pending_idle(void);

static void to_logging(void)
{
	if (s_state == LS_LOGGING) {
		return;
	}
	enum logctl_state prev = s_state;
	cancel_pending_idle();           /* defuse any orphaned idle_finalize_work */
	autooff_cancel();
	power_rail(true);                /* may be coming from dark idle */
	if (prev == LS_IDLE) {
		switched_rail_resume();      /* reverse the IDLE suspend */
		display_set_powered(true);   /* SSD1306 lost its config when rail cycled */
	}
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
	/* Suspend bus drivers BEFORE the rail goes off so their pinctrl-sleep
	 * state lands (pads disconnected, no pulls). If we cut the rail first
	 * the pads stay driven into unpowered slaves -> mA leakage. */
	switched_rail_suspend();
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
		display_notice("sleeping", IDLE_WARN_MS);
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
	switched_rail_resume();          /* reverse the IDLE bus-driver suspend */
	display_set_powered(true);       /* SSD1306 re-init: rail just came back */
	ble_stream_set_adv(true);
	power_batt_sample();
	s_state = LS_STOPPED;
	if (!s_usb) {
		/* Woken by a button press on battery (not USB plug-in). Arm
		 * the auto-sleep timer so the device dozes off again if the
		 * user walked away without starting a recording. Any
		 * subsequent button -> LOGGING / OFFLOAD / USB plug-in
		 * cancels via the standard autooff_cancel call sites. */
		autooff_arm();
		LOG_INF("-> STOPPED (woke from idle, auto-sleep armed)");
	} else {
		LOG_INF("-> STOPPED (woke from idle, USB)");
	}
}

static void to_offload(void)
{
	if (s_state == LS_OFFLOAD) {
		return;
	}
	enum logctl_state prev = s_state;
	cancel_pending_idle();           /* defuse any orphaned idle_finalize_work */
	autooff_cancel();
	power_rail(true);                /* SD needs the rail (may be idle) */
	if (prev == LS_IDLE) {
		switched_rail_resume();      /* reverse the IDLE bus-driver suspend */
		display_set_powered(true);   /* SSD1306 lost its config when rail cycled */
	}
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
		if (s_state == LS_IDLE) {
			/* Wake from sleep on any button press. Stay in STOPPED;
			 * a second REC press is needed to actually start a
			 * session. Avoids "press, see the screen come up mid-
			 * record, didn't mean to start" surprise. */
			wake_idle();
			break;
		}
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
	case EV_WAKE:
		cancel_pending_idle();
		if (s_state == LS_IDLE) {
			wake_idle();
		}
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
		/* Only transition to LOGGING if we're actually exiting
		 * OFFLOAD. controls_init posts this event at boot to seed
		 * the offload switch's resting level into logctl; if we're
		 * not in OFFLOAD that "exit" is meaningless and the
		 * unconditional to_logging() call would override the
		 * post-OTA-boot STOPPED state. */
		if (s_state == LS_OFFLOAD) {
			to_logging();
		}
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
		} else if (s_state == LS_LOGGING) {
			/* The board's 3V3 source switches battery->VBUS on plug-
			 * in; the brief input dip browns out the SSD1306 charge
			 * pump the same way the unplug transient does (see
			 * EV_USB_OFF below). Re-issue the init sequence so the
			 * panel comes back. wake_idle() already does this on
			 * the LS_IDLE branch.
			 *
			 * The SAADC reading of VDDH was the battery voltage
			 * (~3.7 V) before the switch; now VDDH = VBUS (~5 V)
			 * via the regulator. Re-sample so the BAT/USB line in
			 * the UI reflects the new source. Costs ~50 ms of
			 * fast-channel samples (one drop). */
			display_set_powered(true);
			power_batt_refresh_during_acq();
		} else if (s_state == LS_OFFLOAD) {
			display_set_powered(true);   /* charge-pump rescue */
			power_batt_sample();          /* SAADC free in OFFLOAD */
		}
		break;
	case EV_USB_OFF:
		s_usb = false;
		if (s_state == LS_STOPPED || s_state == LS_OFFLOAD) {
			/* Not recording: go dark on battery (3 s notice first). */
			power_batt_sample();   /* SAADC free; record true batt voltage */
			enter_idle();
		} else if (s_state == LS_LOGGING) {
			/* Keep recording. But: the on-board 3V3 regulator dips
			 * briefly when its input switches from VBUS to battery,
			 * and the dip propagates through the P0.13 gate to the
			 * SSD1306 VCC. The chip's I2C stays alive (acks fine)
			 * but its charge pump (which generates the ~7-9 V the
			 * panel needs) collapses, and the panel goes dark even
			 * though our render thread is still writing GDDRAM.
			 * Replaying the driver's init sequence -- specifically
			 * the 8Dh 14h charge-pump-enable command -- brings the
			 * panel back. Writer thread is unaffected (i2c0 / SAADC
			 * / SD are on the same rail and the rail is back up by
			 * the time we get here; if any of those needed re-arm
			 * we'd see fs_write / acquisition errors, which we
			 * haven't). */
			LOG_INF("USB removed during LOGGING -> reinit OLED + refresh batt");
			display_set_powered(true);
			/* VDDH = battery now (~3.7 V from LiPo) instead of VBUS
			 * (~5 V). Without this refresh the cached s_batt_mv is
			 * stale at the USB-era value and the BAT line shows
			 * the wrong voltage until the session stops. Drops
			 * ~50 ms of fast samples. */
			power_batt_refresh_during_acq();
		}
		break;
	case EV_AUTOOFF:
		/* Inactivity timer expired in LS_STOPPED on battery -> sleep.
		 * Re-validate state and USB at fire time: the timer is armed
		 * unconditionally by stop_recording / wake_idle but the user
		 * could have plugged USB in or started recording during the
		 * delay; in those cases the cancel call covered us but a
		 * double-check is cheap and the symptom of getting it wrong
		 * (display dark mid-USB) is the very regression we just
		 * cleaned up. */
		if (s_state == LS_STOPPED && !s_usb) {
			LOG_INF("auto-sleep: %u s STOPPED on battery -> IDLE",
				config_get()->autooff_s);
			enter_idle();
		}
		break;
	case EV_LOW_BATT:
		LOG_WRN("low battery (%d mV)", power_batt_mv());
		/* Do NOT stop the recording or change mode. Two reasons:
		 *
		 * 1. POFWARN-VDDH trips on transient dips too -- in particular
		 *    on every USB-to-battery handoff -- and the previous
		 *    "stop_recording() + enter_idle()" reaction cut the rail
		 *    and blanked the display whenever the user unplugged USB
		 *    while logging. That's exactly the regression the user is
		 *    seeing: logs stop, screen goes dark, no warning.
		 *
		 * 2. POFWARN is a "you have a few seconds of hold-up left"
		 *    notice, not "you're about to brown out right now." We
		 *    storage_sync()'d at every slow boundary, so the latest
		 *    full sector is on the card; if the chip actually browns
		 *    out we lose at most the sub-sector tail. Letting the
		 *    user keep recording until VDD genuinely collapses is the
		 *    better trade than killing the session at the first dip.
		 *
		 * The hardware POR + RAM retention is the safety net. The
		 * user can also explicitly stop with REC short / `pwr stop`. */
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

	/* Don't autostart a recording on the first boot after an OTA
	 * swap. The previous session was interrupted by the upload + reset
	 * and the user is actively interacting (they just pushed firmware);
	 * dropping into LOGGING would clobber whatever they want to do
	 * next. ota_test_pending() returns true exactly when this boot
	 * came up unconfirmed (= just-swapped), which gets cleared when
	 * main() reaches ota_confirm_current() shortly after this call.
	 * On any subsequent reboot the image is confirmed -> autostart
	 * works normally. */
	bool post_ota_boot = ota_test_pending();

	if (post_ota_boot) {
		LOG_INF("post-OTA boot -> STOPPED (skipping autostart)");
		if (!s_usb) {
			logctl_post(EV_USB_OFF);   /* still want idle on battery */
		}
	} else if (config_get()->autostart) {
		logctl_post(EV_LOG_START);
	} else if (s_usb) {
		LOG_INF("autostart off -> STOPPED (use 'pwr log' / REC)");
	} else {
		logctl_post(EV_USB_OFF);   /* battery + no autostart -> dark idle */
	}
}
