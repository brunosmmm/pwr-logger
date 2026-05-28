#ifndef LOGGER_H_
#define LOGGER_H_

#include <stdint.h>

/*
 * Logging pipeline: owns the SD-writer thread that drains acq_msgq + ina228_msgq,
 * encodes TDMS segments, writes to SD, feeds the aggregator, and publishes the
 * BLE live view. Decoupled so acquisition never touches the filesystem.
 */

int logger_init(void);
int logger_start(void);  /* open session file, start acquisition, run writer */
int logger_stop(void);
int logger_rotate(void); /* close current file + open a fresh one, mid-stream */

/* SD-usage / acquisition-rate snapshots. Refreshed by the writer thread
 * (which already holds the FatFs lock for its own fs_write calls), so any
 * other thread can read them without touching the filesystem. Returns 0 if
 * the writer hasn't published a sample yet (boot, between sessions). */
void     logger_sd_usage(uint64_t *file_b, uint64_t *free_b, uint64_t *total_b);
uint32_t logger_samples_per_sec(void);

#endif /* LOGGER_H_ */
