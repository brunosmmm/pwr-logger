/*
 * Vendor-specific bulk-IN USB class for high-throughput device->host transfers.
 * Companion to the CDC ACM shell and the MSC offload interface in the same
 * composite USB device; lets `pwr download` push session bytes at FS bulk
 * speeds (~700 KB/s) instead of the Zephyr CDC ACM single-in-flight cap
 * (~160 KB/s).
 *
 * Usage:
 *   pwr_bulk_send(buf, len);   // blocks if pool is full
 *   ...
 *   pwr_bulk_flush();          // wait until all in-flight transfers complete
 *
 * Host side opens this interface via libusb / pyusb and reads from the bulk-IN
 * endpoint directly. No class driver required on Linux; on Windows a WCID
 * (Microsoft OS) descriptor auto-binds it to WinUSB. See tools/pwrdl.py.
 */
#ifndef USBD_PWR_BULK_H_
#define USBD_PWR_BULK_H_

#include <stddef.h>
#include <stdint.h>

int  pwr_bulk_send(const uint8_t *data, size_t len);
void pwr_bulk_flush(void);

#endif /* USBD_PWR_BULK_H_ */
