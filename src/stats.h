#ifndef STATS_H_
#define STATS_H_

#include <stdint.h>
#include <stdbool.h>

/*
 * Stats engine: rolling (windowed) min/mean/max, latched HOLD peaks, and
 * energy accumulator. Runs in the background regardless of which UI page is
 * visible. Producers (aggregate.c at ~20 Hz for fast channel, ina228.c at
 * ~20 Hz for slow) push samples via callbacks registered at boot; readers
 * (UI page renderers, CLI) snapshot under a mutex.
 *
 * All values in SI units (amps, volts, watts, °C). Energy accumulates in
 * watt-hours; queries that want mAh use the slow_i sample stream directly.
 */

/* Channel identifiers (also index into per-channel arrays). */
enum stats_ch {
	STATS_CH_FAST_I = 0,   /* INA240 fast SAADC current, A */
	STATS_CH_SLOW_I,       /* INA228 current, A */
	STATS_CH_SLOW_V,       /* INA228 vbus, V */
	STATS_CH_SLOW_P,       /* INA228 power, W */
	STATS_CH_SLOW_T,       /* INA228 die temp, °C */
	STATS_CH__COUNT,
};

/* Bit masks for stats_hold_clear(). */
#define STATS_CH_BIT(ch)  (1u << (ch))
#define STATS_CH_ALL      ((1u << STATS_CH__COUNT) - 1u)

/* Ring-buffer depth -- 60 s at 20 Hz. window_s is clamped to this. */
#define STATS_MAX_SAMPLES   1200u
#define STATS_MAX_WINDOW_S  60u

struct stats_window {
	float    min;
	float    mean;
	float    max;
	uint32_t n;       /* samples actually scanned (0 => no data) */
};

struct stats_hold {
	float   min;
	float   max;
	int64_t since_uptime_ms;   /* k_uptime_get() at last clear */
	bool    valid;
};

/* ---- lifecycle ----------------------------------------------------------- */
void stats_init(void);          /* call AFTER aggregate/ina228 module init */
void stats_session_reset(void); /* drop rolling buffers; called from logger_start */

/* ---- rolling window ----------------------------------------------------- */
/* Returns false if window has no samples; out is left untouched.
 * `window_s` is clamped to [1, STATS_MAX_WINDOW_S]; 0 => use STATS_MAX_WINDOW_S. */
bool stats_window_get(enum stats_ch ch, uint16_t window_s,
		      struct stats_window *out);

/* ---- HOLD latches ------------------------------------------------------- */
/* Returns false if the channel has never seen a sample since last clear. */
bool stats_hold_get(enum stats_ch ch, struct stats_hold *out);
void stats_hold_clear(uint8_t channel_mask);  /* mask of STATS_CH_BIT(ch) */

/* ---- energy accumulator ------------------------------------------------- */
double stats_energy_wh(void);
double stats_energy_q_as(void);   /* ∫I·dt in ampere-seconds (= coulombs) */
int64_t stats_energy_elapsed_ms(void);
void   stats_energy_clear(void);

#endif /* STATS_H_ */
