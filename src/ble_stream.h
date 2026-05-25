#ifndef BLE_STREAM_H_
#define BLE_STREAM_H_

#include "aggregate.h"
#include "ina228.h"

/*
 * BLE live view: a multi-rate, tagged notification stream. Each notification is
 * one self-contained record with a leading type byte:
 *   0x01 FAST   - fast-channel aggregate window (~20 Hz, raw codes)
 *   0x02 SLOW   - INA228 sample in real units (V/A/W/degC, its native rate)
 *   0x03 STATUS - device state + battery + drops (~2 Hz)
 * The central demuxes by tag; each channel streams at its own cadence.
 */

int ble_stream_init(void);
int ble_stream_publish(const struct agg_window *w);          /* FAST */
int ble_stream_publish_slow(const struct ina228_sample *s);  /* SLOW (INA228) */
void ble_stream_set_adv(bool on);   /* start/stop advertising (off in dark idle) */

#endif /* BLE_STREAM_H_ */
