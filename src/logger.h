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

/* Post a marker request. The writer thread will append a TDMS root property
 * (marker_<N> = "<UTC>  <msg>") at the next loop iteration so it shows up
 * in the file alongside the data. Safe to call from any thread; non-
 * blocking; silently dropped if the marker queue is full (>=8 pending).
 * Returns 0 on enqueue, -EBUSY if dropped. */
int logger_marker(const char *msg);

#endif /* LOGGER_H_ */
