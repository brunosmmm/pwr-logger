#ifndef BLE_STREAM_H_
#define BLE_STREAM_H_

#include "aggregate.h"

/*
 * BLE aggregated live view (separate, compact schema; see DATA_FORMAT.md sect 7).
 * Also the OTA/DFU transport (mcumgr SMP over BLE).
 */

int ble_stream_init(void);
int ble_stream_publish(const struct agg_window *w);

#endif /* BLE_STREAM_H_ */
