#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "timebase.h"
#include "storage.h"
#include "acquisition.h"
#include "ina228.h"
#include "ble_stream.h"
#include "usb_msc.h"
#include "logger.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

enum app_mode {
	MODE_IDLE,
	MODE_LOGGING,
	MODE_OFFLOAD,
};

int main(void)
{
	static enum app_mode mode = MODE_IDLE;

	LOG_INF("pwr-logger starting");

	timebase_init();
	if (storage_mount() != 0) {
		LOG_ERR("SD mount failed; check card / wiring");
	}
	acq_init();
	ina228_init();
	ble_stream_init();
	usb_storage_init();
	logger_init();

	/* TODO: real mode state machine driven by button / BLE command / USB
	 * attach. LOGGING and OFFLOAD are mutually exclusive at the FS level. */
	mode = MODE_LOGGING;
	logger_start();
	LOG_INF("mode = %d", (int)mode);

	while (1) {
		k_sleep(K_SECONDS(1));
	}
	return 0;
}
