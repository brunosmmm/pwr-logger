#ifndef TIMEBASE_H_
#define TIMEBASE_H_

#include <stdint.h>

/*
 * Master time base: one free-running HFCLK TIMER @ 1 MHz (1 us ticks) that both
 * streams are hardware-stamped against via PPI. See DATA_FORMAT.md section 1.
 */

int timebase_init(void);

/* Current raw 32-bit master tick. */
uint32_t timebase_now32(void);

/* Extend a captured 32-bit tick to monotonic 64 bits. Call from ONE thread
 * (the logger writer) which sees ticks far faster than the ~71 min wrap. */
uint64_t timebase_extend(uint32_t t32);

#endif /* TIMEBASE_H_ */
