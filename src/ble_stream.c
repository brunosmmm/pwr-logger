#include "ble_stream.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_stream, LOG_LEVEL_INF);

/*
 * TODO:
 *  - bt_enable() and start connectable advertising.
 *  - Define a GATT service for the aggregated live view: decimated waveform
 *    preview + per-window RMS/avg/min/max + status, plus control/alarm chars
 *    (DATA_FORMAT.md section 7). Use 2M PHY + larger MTU for throughput.
 *  - OTA rides the same link via mcumgr SMP-over-BLE (enabled in prj.conf).
 */

int ble_stream_init(void)
{
	LOG_WRN("ble_stream_init: TODO bt_enable + GATT live-view service + adv");
	return 0;
}

int ble_stream_publish(const struct agg_window *w)
{
	ARG_UNUSED(w);
	/* TODO: notify subscribers with the window summary. */
	return 0;
}
