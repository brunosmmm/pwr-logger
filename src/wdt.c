/*
 * Hardware watchdog for production safety.
 *
 * The nRF52840 has a single WDT peripheral. Zephyr exposes it via
 * <zephyr/drivers/watchdog.h>. We install one timeout channel and feed
 * it from the system workqueue every WDT_FEED_MS; if anything wedges
 * sysworkq for more than WDT_TIMEOUT_MS the SoC resets via WDT.
 *
 * Why sysworkq instead of a dedicated thread: it's where all the other
 * "periodic small job" work already runs (logger periodic flush,
 * display refresh, BLE timers). If sysworkq stops, we've already lost
 * a lot of the device's normal behavior -- that's a strong signal
 * that resetting is the right move.
 *
 * Limitation: a stuck thread other than sysworkq (e.g. logger writer
 * blocked on a never-finished SD write) does not get caught here. The
 * device keeps responding to USB, BLE, the shell -- it just stops
 * recording. Catching that needs a per-thread heartbeat pattern;
 * deferred until we observe it in the field.
 *
 * Sleep behaviour: we do NOT pass WDT_OPT_PAUSE_IN_SLEEP. With the
 * pause flag, the WDT stops counting when the CPU is in WFI; an
 * actually-deadlocked system goes idle from the kernel's perspective
 * and would not be reset. Letting the WDT run continuously means even
 * a "everything is blocked on semaphores" deadlock trips it after the
 * timeout. For our deep-sleep (sys_poweroff) path the SoC resets on
 * wake anyway, so WDT continuity across that is moot.
 *
 * Debugger handling: WDT_OPT_PAUSE_HALTED_BY_DBG so a `halt` in
 * OpenOCD / pyocd doesn't reboot the board on you.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>

#include "wdt.h"

#define WDT_TIMEOUT_MS  4000   /* SoC reset if not fed within this long */
#define WDT_FEED_MS     1000   /* feed cadence on sysworkq */

static const struct device *const wdt_dev =
	DEVICE_DT_GET(DT_NODELABEL(wdt0));
static int wdt_channel = -1;

static void wdt_feed_work(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(wdt_dwork, wdt_feed_work);

static void wdt_feed_work(struct k_work *w)
{
	ARG_UNUSED(w);
	if (wdt_channel >= 0) {
		(void)wdt_feed(wdt_dev, wdt_channel);
	}
	k_work_schedule(&wdt_dwork, K_MSEC(WDT_FEED_MS));
}

void wdt_feed_now(void)
{
	if (wdt_channel >= 0) {
		(void)wdt_feed(wdt_dev, wdt_channel);
	}
}

int wdt_init(void)
{
	if (!device_is_ready(wdt_dev)) {
		return -ENODEV;
	}

	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0U, .max = WDT_TIMEOUT_MS },
		.callback = NULL,        /* hard reset, no pre-reset callback */
		.flags = WDT_FLAG_RESET_SOC,
	};

	wdt_channel = wdt_install_timeout(wdt_dev, &cfg);
	if (wdt_channel < 0) {
		return wdt_channel;
	}

	int rc = wdt_setup(wdt_dev, WDT_OPT_PAUSE_HALTED_BY_DBG);

	if (rc < 0) {
		wdt_channel = -1;
		return rc;
	}

	k_work_schedule(&wdt_dwork, K_MSEC(WDT_FEED_MS));
	return 0;
}
