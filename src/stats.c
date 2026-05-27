#include "stats.h"
#include "aggregate.h"
#include "ina228.h"
#include "app_config.h"

#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(stats, LOG_LEVEL_INF);

/* ---- scaling constants (mirror logger.c / TDMS scales) ------------------ */
/* int16 SAADC code -> amps. Derived once at compile time. */
#define FAST_SLOPE_A    ((float)((double)ADC_VFULL_SCALE_V / \
			((double)(1u << ADC_BITS) * \
			 (double)INA240_GAIN * (double)INA240_SHUNT_OHMS)))
#define FAST_INTCEPT_A  ((float)(-(double)INA240_VREF_V / \
			((double)INA240_GAIN * (double)INA240_SHUNT_OHMS)))

#define INA228_VBUS_LSB_V     (195.3125e-6f)         /* fixed silicon */
#define INA228_TEMP_LSB_C     (7.8125e-3f)           /* fixed silicon */
#define INA228_CURR_LSB_A     ((float)((double)INA228_IMAX_A / 524288.0))
#define INA228_POWER_LSB_W    (3.2f * INA228_CURR_LSB_A)

/* ---- per-channel state -------------------------------------------------- */
struct chan {
	float    buf[STATS_MAX_SAMPLES];
	uint16_t head;   /* next write index */
	uint16_t fill;   /* count of valid samples, capped at STATS_MAX_SAMPLES */

	/* HOLD latches */
	float    hmin, hmax;
	int64_t  hsince_ms;
	bool     hvalid;
};

static struct chan s_chan[STATS_CH__COUNT];

/* energy / charge accumulators */
static double  s_energy_wh;
static double  s_q_as;
static int64_t s_e_since_ms;
static int64_t s_last_slow_ms;   /* for dt on slow updates */

static struct k_mutex s_lock;

/* ---- internal helpers --------------------------------------------------- */
static void push_locked(enum stats_ch ch, float v)
{
	struct chan *c = &s_chan[ch];

	c->buf[c->head] = v;
	c->head = (c->head + 1u) % STATS_MAX_SAMPLES;
	if (c->fill < STATS_MAX_SAMPLES) {
		c->fill++;
	}

	if (!c->hvalid) {
		c->hmin = c->hmax = v;
		c->hsince_ms = k_uptime_get();
		c->hvalid = true;
	} else {
		if (v < c->hmin) c->hmin = v;
		if (v > c->hmax) c->hmax = v;
	}
}

/* ---- producer callbacks (run in producer-thread context) ---------------- */
static void on_fast_window(const struct agg_window *w)
{
	/* Scale codes to amps via the same FAST linear scale TDMS writes. */
	float i_avg = (float)w->avg * FAST_SLOPE_A + FAST_INTCEPT_A;
	float i_min = (float)w->min * FAST_SLOPE_A + FAST_INTCEPT_A;
	float i_max = (float)w->max * FAST_SLOPE_A + FAST_INTCEPT_A;

	k_mutex_lock(&s_lock, K_FOREVER);
	/* Single-window push: the agg window already contains a stat summary.
	 * We push the avg into the rolling buffer (rolling stats = stats over
	 * recent window-avgs), but update HOLD with the window's min/max so a
	 * brief transient inside the 50 ms window still gets caught. */
	struct chan *c = &s_chan[STATS_CH_FAST_I];
	c->buf[c->head] = i_avg;
	c->head = (c->head + 1u) % STATS_MAX_SAMPLES;
	if (c->fill < STATS_MAX_SAMPLES) c->fill++;
	if (!c->hvalid) {
		c->hmin = i_min;
		c->hmax = i_max;
		c->hsince_ms = k_uptime_get();
		c->hvalid = true;
	} else {
		if (i_min < c->hmin) c->hmin = i_min;
		if (i_max > c->hmax) c->hmax = i_max;
	}
	k_mutex_unlock(&s_lock);
}

static void on_slow_sample(const struct ina228_sample *s)
{
	float v = (float)s->vbus_raw    * INA228_VBUS_LSB_V;
	float i = (float)s->current_raw * INA228_CURR_LSB_A;
	float p = (float)s->power_raw   * INA228_POWER_LSB_W;
	float t = (float)s->temp_raw    * INA228_TEMP_LSB_C;
	int64_t now = k_uptime_get();

	k_mutex_lock(&s_lock, K_FOREVER);
	push_locked(STATS_CH_SLOW_I, i);
	push_locked(STATS_CH_SLOW_V, v);
	push_locked(STATS_CH_SLOW_P, p);
	push_locked(STATS_CH_SLOW_T, t);

	/* Energy: dE = P * dt; dQ = I * dt. Skip the first sample (no dt). */
	if (s_last_slow_ms > 0) {
		double dt = (double)(now - s_last_slow_ms) * 1e-3;   /* sec */
		if (dt > 0.0 && dt < 1.0) {   /* clamp huge gaps as bad data */
			s_energy_wh += (double)p * dt / 3600.0;
			s_q_as      += (double)i * dt;
		}
	} else {
		s_e_since_ms = now;
	}
	s_last_slow_ms = now;
	k_mutex_unlock(&s_lock);
}

/* ---- public API --------------------------------------------------------- */
void stats_init(void)
{
	k_mutex_init(&s_lock);
	memset(s_chan, 0, sizeof(s_chan));
	s_energy_wh = 0.0;
	s_q_as = 0.0;
	s_e_since_ms = 0;
	s_last_slow_ms = 0;

	agg_set_window_cb(on_fast_window);
	ina228_set_sample_cb(on_slow_sample);
	LOG_INF("stats: up (5 channels, %u-sample buffer, %u s max window)",
		STATS_MAX_SAMPLES, STATS_MAX_WINDOW_S);
}

void stats_session_reset(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	memset(s_chan, 0, sizeof(s_chan));
	k_mutex_unlock(&s_lock);
}

bool stats_window_get(enum stats_ch ch, uint16_t window_s,
		      struct stats_window *out)
{
	if (ch >= STATS_CH__COUNT || out == NULL) {
		return false;
	}
	if (window_s == 0 || window_s > STATS_MAX_WINDOW_S) {
		window_s = STATS_MAX_WINDOW_S;
	}
	/* 20 Hz sample rate -> window samples. Add a small head-room margin
	 * so a 1 s window sees ~20 samples even if drift puts it at 19. */
	uint32_t want = (uint32_t)window_s * 20u;

	k_mutex_lock(&s_lock, K_FOREVER);
	const struct chan *c = &s_chan[ch];
	uint32_t avail = c->fill;
	uint32_t n = (want < avail) ? want : avail;
	if (n == 0) {
		k_mutex_unlock(&s_lock);
		return false;
	}
	/* Walk back n samples from head. */
	double sum = 0.0;
	float mn, mx;
	uint16_t idx = (c->head + STATS_MAX_SAMPLES - 1u) % STATS_MAX_SAMPLES;
	mn = mx = c->buf[idx];
	for (uint32_t k = 0; k < n; k++) {
		float v = c->buf[idx];
		sum += (double)v;
		if (v < mn) mn = v;
		if (v > mx) mx = v;
		idx = (idx + STATS_MAX_SAMPLES - 1u) % STATS_MAX_SAMPLES;
	}
	out->min  = mn;
	out->max  = mx;
	out->mean = (float)(sum / (double)n);
	out->n    = n;
	k_mutex_unlock(&s_lock);
	return true;
}

bool stats_hold_get(enum stats_ch ch, struct stats_hold *out)
{
	if (ch >= STATS_CH__COUNT || out == NULL) {
		return false;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	const struct chan *c = &s_chan[ch];
	if (!c->hvalid) {
		k_mutex_unlock(&s_lock);
		return false;
	}
	out->min = c->hmin;
	out->max = c->hmax;
	out->since_uptime_ms = c->hsince_ms;
	out->valid = true;
	k_mutex_unlock(&s_lock);
	return true;
}

void stats_hold_clear(uint8_t mask)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	for (int i = 0; i < STATS_CH__COUNT; i++) {
		if (mask & STATS_CH_BIT(i)) {
			s_chan[i].hvalid = false;
		}
	}
	k_mutex_unlock(&s_lock);
}

double stats_energy_wh(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	double v = s_energy_wh;
	k_mutex_unlock(&s_lock);
	return v;
}

double stats_energy_q_as(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	double v = s_q_as;
	k_mutex_unlock(&s_lock);
	return v;
}

int64_t stats_energy_elapsed_ms(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	int64_t s = s_e_since_ms;
	int64_t now = k_uptime_get();
	k_mutex_unlock(&s_lock);
	return (s == 0) ? 0 : (now - s);
}

void stats_energy_clear(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	s_energy_wh = 0.0;
	s_q_as = 0.0;
	s_e_since_ms = 0;
	s_last_slow_ms = 0;
	k_mutex_unlock(&s_lock);
}
