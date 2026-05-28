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

#endif /* WDT_H_ */
