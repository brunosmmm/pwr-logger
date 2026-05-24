#ifndef LOGGER_H_
#define LOGGER_H_

/*
 * Logging pipeline: owns the SD-writer thread that drains acq_msgq + ina228_msgq,
 * encodes TDMS segments, writes to SD, feeds the aggregator, and publishes the
 * BLE live view. Decoupled so acquisition never touches the filesystem.
 */

int logger_init(void);
int logger_start(void);  /* open session file, start acquisition, run writer */
int logger_stop(void);

#endif /* LOGGER_H_ */
