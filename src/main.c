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
	acq_init();
	ina228_init();
	ble_stream_init();
	usb_storage_init();
	logger_init();

	/* TODO: real mode state machine (button / BLE command / USB attach);
	 * LOGGING and OFFLOAD are mutually exclusive at the FS level. */
	logger_start();

	while (1) {
		k_sleep(K_SECONDS(1));
	}
#endif
	return 0;
}
