#ifndef BRINGUP_H_
#define BRINGUP_H_

/*
 * Phase 0/1 hardware bring-up probe. Heartbeat LED + a periodic self-test that
 * reports what it can see (INA228 over I2C, SD card, SAADC on AIN7), so wiring
 * each component can be verified live over the console/RTT. Does not return.
 */
void bringup_run(void);

#endif /* BRINGUP_H_ */
