#ifndef TIMEBASE_H_
#define TIMEBASE_H_

#include <stdint.h>

/*
 * Master time base: one free-running HFCLK TIMER @ 1 MHz (1 us ticks) that both
 * streams are hardware-stamped against via PPI. See DATA_FORMAT.md section 1.
 */

/* One-time setup: configure prescaler / bit width. Does NOT start the
 * timer (TIMER2 backed by HFCLK pins HFXO on; only run it when actually
 * sampling). Safe to call before timebase_start. */
int timebase_init(void);

/* Start TIMER2 (acquisition starting). Resets wrap-extension state and
 * clears the counter. Idempotent. Call from acq_start. */
void timebase_start(void);

/* Stop TIMER2 (acquisition stopping). The timer stops counting and
 * releases the HFCLK request -- saves ~500-700 µA. Reads of
 * timebase_now32() after stop return stale (last captured) values;
 * don't call it outside the acq/ina228 paths. Idempotent. */
void timebase_stop(void);

/* Current raw 32-bit master tick. Only meaningful while timebase_start()
 * has been called more recently than timebase_stop(). */
uint32_t timebase_now32(void);

/* Extend a captured 32-bit tick to monotonic 64 bits. Call from ONE thread
 * (the logger writer) which sees ticks far faster than the ~71 min wrap. */
uint64_t timebase_extend(uint32_t t32);

#endif /* TIMEBASE_H_ */
