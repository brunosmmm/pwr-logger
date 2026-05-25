#ifndef WALLCLOCK_H_
#define WALLCLOCK_H_

#include <time.h>
#include <stdbool.h>

/*
 * Tier-1 software wall clock: wall = epoch_base + k_uptime (LFXO-backed, survives
 * System-ON dark idle; lost across System OFF / power loss -> re-sync). No
 * external RTC. The last-known time is persisted to settings periodically and on
 * clean stop so a reset recovers an estimate, corrected on the next real sync.
 */

void   clk_init(void);    /* defaults; call BEFORE settings_load */
void   clk_start(void);   /* call AFTER settings_load: restore estimate + periodic persist */
time_t clk_now(void);     /* epoch seconds, 0 if no time at all */
bool   clk_is_synced(void); /* true only after a real clk_set() this session */
bool   clk_has_time(void);  /* true if synced OR a persisted estimate was restored */
void   clk_set(time_t t); /* real time sync */
void   clk_persist(void); /* save current estimate now (e.g. on clean stop) */

#endif /* WALLCLOCK_H_ */
