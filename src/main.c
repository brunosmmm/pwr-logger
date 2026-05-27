#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "timebase.h"

#ifdef CONFIG_APP_BRINGUP
#include "bringup.h"
#else
#include "acquisition.h"
#include "ina228.h"
#include "ble_stream.h"
#include "usb_msc.h"
#include "logger.h"
#include "logctl.h"
#include "config.h"
#include "clock.h"
#include "power.h"
#include "display.h"
#include "controls.h"
#include "stats.h"
#include "ui.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("pwr-logger starting");
	timebase_init();

#ifdef CONFIG_APP_BRINGUP
	/* Phase 0/1: hardware bring-up self-test (does not return). */
	bringup_run();
#else
	/* Persistent config + wall clock first: clk_init() sets defaults before
	 * config_load() runs settings_load() (which fills both via static
	 * handlers); clk_start() then restores the time estimate. */
	clk_init();
	config_load();
	clk_start();

	/* Bring the 3.3 V rail up before anything that needs it (INA228/INA240/SD). */
	power_init();

	acq_init();
	ina228_init();
	ble_stream_init();
	usb_storage_init();
	display_init();   /* no-op unless CONFIG_APP_DISPLAY */
	ui_init();        /* multi-page UI state (no-op without DISPLAY) */
	stats_init();     /* hooks aggregate.c + ina228.c callbacks; lock-light */
	logger_init();

	/* logctl owns the mode (logging / offload / ship), driven by the console,
	 * BLE, buttons/switch and USB/battery events. It seeds the initial mode
	 * from config (autostart) here. */
	logctl_init();

	/* Front-panel inputs + LED (must come AFTER logctl_init -- controls
	 * posts the boot-time OFFLOAD switch level into logctl's msgq). */
	controls_init();

	while (1) {
		k_sleep(K_SECONDS(1));
	}
#endif
	return 0;
}
