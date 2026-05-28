#ifndef WDT_H_
#define WDT_H_

/* Hardware watchdog: installs an nRF WDT timeout and starts a delayed
 * work that re-feeds it every WDT_FEED_MS milliseconds. If the system
 * deadlocks badly enough that sysworkq stops running for WDT_TIMEOUT_MS,
 * the SoC resets cleanly via the WDT.
 *
 * Returns 0 on success, negative errno on failure. Failures are
 * non-fatal (the rest of the app keeps running without a watchdog). */
int wdt_init(void);

/* Feed the watchdog from arbitrary context. The periodic sysworkq feed
 * is enough for normal operation, but long synchronous operations that
 * busy-block sysworkq (notably OTA flash writes -- ~250 KB at SPI flash
 * write speed > the 4 s WDT window) need to feed inline. Safe to call
 * before wdt_init() (no-op then). */
void wdt_feed_now(void);

#endif /* WDT_H_ */
