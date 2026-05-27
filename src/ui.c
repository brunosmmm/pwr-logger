#include "ui.h"

#if defined(CONFIG_APP_DISPLAY)

#include "config.h"
#include "clock.h"
#include "logctl.h"
#include "power.h"
#include "stats.h"
#include "storage.h"
#include "acquisition.h"
#include "ina228.h"
#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ui, LOG_LEVEL_INF);

/* Font height of the default 10x16 font (cached at first render). */
static uint8_t s_fh = 16;

/* Channel auto-cycle on PEAKS / HOLD pages. */
#define UI_CYCLE_FREEZE_MS  10000     /* pause auto-cycle for N ms after a kick */

/* Visible-channel cycle for PEAKS / HOLD. Order matches user expectation: I->V->P. */
static const enum stats_ch s_cycle[] = {
	STATS_CH_SLOW_I, STATS_CH_SLOW_V, STATS_CH_SLOW_P,
};
#define UI_CYCLE_N  ((uint8_t)(sizeof(s_cycle) / sizeof(s_cycle[0])))

/* ---- shared UI state (writers + display thread reader -> mutex) ---------- */
static enum ui_page s_page = PAGE_LIVE;
static int64_t      s_last_input_ms;
static struct k_mutex s_lock;

void ui_init(void)
{
	k_mutex_init(&s_lock);
	s_last_input_ms = k_uptime_get();
	s_page = (enum ui_page)0;   /* default LIVE; config-driven default lands in Phase 4 */
}

static enum ui_page page_get(void)
{
	enum ui_page p;
	k_mutex_lock(&s_lock, K_FOREVER); p = s_page; k_mutex_unlock(&s_lock);
	return p;
}

void ui_next_page(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	s_page = (enum ui_page)(((int)s_page + 1) % PAGE__COUNT);
	s_last_input_ms = k_uptime_get();
	k_mutex_unlock(&s_lock);
}
void ui_prev_page(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	s_page = (enum ui_page)(((int)s_page + PAGE__COUNT - 1) % PAGE__COUNT);
	s_last_input_ms = k_uptime_get();
	k_mutex_unlock(&s_lock);
}
void ui_set_page(enum ui_page p)
{
	if ((unsigned)p >= PAGE__COUNT) return;
	k_mutex_lock(&s_lock, K_FOREVER);
	s_page = p;
	s_last_input_ms = k_uptime_get();
	k_mutex_unlock(&s_lock);
}
void ui_kick(void)
{
	k_mutex_lock(&s_lock, K_FOREVER); s_last_input_ms = k_uptime_get(); k_mutex_unlock(&s_lock);
}
enum ui_page ui_get_page(void) { return page_get(); }

void ui_hold_clear_current(void)
{
	enum ui_page p = page_get();
	if (p == PAGE_HOLD) {
		/* Determine current channel (the auto-cycle pick at this moment). */
		int64_t since;
		k_mutex_lock(&s_lock, K_FOREVER); since = s_last_input_ms; k_mutex_unlock(&s_lock);
		int64_t now = k_uptime_get();
		uint16_t cycle = config_get()->stats_window_s ? config_get()->stats_window_s : 10;
		(void)since; (void)now; (void)cycle;
		/* For Phase 2 simplicity, clear all HOLD channels on any HOLD-page long-press. */
		stats_hold_clear(STATS_CH_ALL);
	}
}

/* ---- format helpers (lifted from display.c) ----------------------------- */
static const char *state_abbr3(int s)
{
	switch (s) {
	case 1: return "REC";
	case 2: return "OFL";
	case 3: return "SHP";
	case 4: return "IDL";
	default: return "STP";
	}
}

/* µ-prefix auto-scale: |val| in µ-units of `unit` (A, W). */
static void fmt_micro(char *out, size_t sz, int32_t uval, char u)
{
	int neg = uval < 0;
	uint32_t a = neg ? (uint32_t)(-uval) : (uint32_t)uval;
	if (a >= 1000000u) {
		uint32_t w = a / 1000000u, f = (a % 1000000u) / 10000u;
		snprintf(out, sz, "%s%u.%02u %c", neg ? "-" : "", w, f, u);
	} else if (a >= 1000u) {
		uint32_t w = a / 1000u, f = (a % 1000u) / 10u;
		snprintf(out, sz, "%s%u.%02u m%c", neg ? "-" : "", w, f, u);
	} else {
		snprintf(out, sz, "%s%u u%c", neg ? "-" : "", a, u);
	}
}
/* m-prefix auto-scale: val in milli-units (e.g. mV). */
static void fmt_milli(char *out, size_t sz, int32_t mval, char u)
{
	int neg = mval < 0;
	uint32_t a = neg ? (uint32_t)(-mval) : (uint32_t)mval;
	if (a >= 1000u) {
		uint32_t w = a / 1000u, f = (a % 1000u) / 10u;
		snprintf(out, sz, "%s%u.%02u %c", neg ? "-" : "", w, f, u);
	} else {
		snprintf(out, sz, "%s%u m%c", neg ? "-" : "", a, u);
	}
}

/* Format a float in SI units, picking the prefix (µ/m/-) by magnitude. */
static void fmt_si_f(char *out, size_t sz, float v, char unit)
{
	float a = v < 0 ? -v : v;
	if (a >= 1.0f) {
		snprintf(out, sz, "%.2f %c", (double)v, unit);
	} else if (a >= 1e-3f) {
		snprintf(out, sz, "%.2f m%c", (double)(v * 1000.0f), unit);
	} else {
		snprintf(out, sz, "%.1f u%c", (double)(v * 1e6f), unit);
	}
}

/* Map stats_ch -> label / unit char for compact rendering. */
static const char *ch_label(enum stats_ch ch)
{
	switch (ch) {
	case STATS_CH_FAST_I: return "If";
	case STATS_CH_SLOW_I: return "I";
	case STATS_CH_SLOW_V: return "V";
	case STATS_CH_SLOW_P: return "P";
	case STATS_CH_SLOW_T: return "T";
	default: return "?";
	}
}
static char ch_unit(enum stats_ch ch)
{
	switch (ch) {
	case STATS_CH_FAST_I: case STATS_CH_SLOW_I: return 'A';
	case STATS_CH_SLOW_V: return 'V';
	case STATS_CH_SLOW_P: return 'W';
	case STATS_CH_SLOW_T: return 'C';
	default: return '?';
	}
}

/* Current PEAKS/HOLD auto-cycle channel given uptime + last-input freeze. */
static enum stats_ch cycle_channel(uint8_t cycle_s)
{
	int64_t now = k_uptime_get();
	int64_t last; k_mutex_lock(&s_lock, K_FOREVER); last = s_last_input_ms; k_mutex_unlock(&s_lock);
	int64_t base = (now - last) < UI_CYCLE_FREEZE_MS ? last + UI_CYCLE_FREEZE_MS : now;
	uint32_t step = cycle_s ? cycle_s : 4;
	uint8_t idx = (uint8_t)(((uint64_t)base / (step * 1000ULL)) % UI_CYCLE_N);
	return s_cycle[idx];
}

/* ---- status bar (yellow strip @ y=0..15, inverted) ---------------------- */
static void render_status_bar(const struct device *disp, const char *primary)
{
	/* Layout: [STATE]  [primary text]  e.g. "REC  USB"  or  "REC  ROLL 10s". */
	char bar[20];
	const char *st = state_abbr3(logctl_state());
	bool usb = power_usb_present();
	int mv = power_batt_mv();

	if (primary && *primary) {
		snprintf(bar, sizeof(bar), "%-3s %s", st, primary);
	} else if (usb) {
		snprintf(bar, sizeof(bar), "%-3s USB", st);
	} else if (mv > 0) {
		snprintf(bar, sizeof(bar), "%-3s BAT %d.%dV", st, mv/1000, (mv%1000)/100);
	} else {
		snprintf(bar, sizeof(bar), "%-3s BAT", st);
	}
	cfb_print(disp, bar, 0, 0);
	/* Invert the entire yellow strip -> bright background, dark text. */
	cfb_invert_area(disp, 0, 0, 128, 16);
}

/* ---- per-page renderers (assume framebuffer already cleared) ------------ */
static void page_live(const struct device *disp)
{
	render_status_bar(disp, NULL);

	struct ina228_sample s;
	char li[16], lv[16], lp[16], buf[12];
	if (ina228_last(&s)) {
		/* Reuse the existing display.c scaling math via the same constants. */
		extern const float INA228_VBUS_LSB_V_DUMMY;   /* unused -- formulas below inline scales */
		int v_mv = (int)((float)s.vbus_raw   * 0.1953125f);   /* INA228 VBUS LSB = 195.3125 uV */
		int i_ua = (int)((float)s.current_raw * (20.0f / 524288.0f) * 1e6f);
		int p_uw = (int)((float)s.power_raw  * 3.2f * (20.0f / 524288.0f) * 1e6f);
		(void)INA228_VBUS_LSB_V_DUMMY;
		fmt_micro(buf, sizeof(buf), i_ua, 'A'); snprintf(li, sizeof(li), "I: %s", buf);
		fmt_milli(buf, sizeof(buf), v_mv, 'V'); snprintf(lv, sizeof(lv), "V: %s", buf);
		fmt_micro(buf, sizeof(buf), p_uw, 'W'); snprintf(lp, sizeof(lp), "P: %s", buf);
	} else {
		snprintf(li, sizeof(li), "I: --");
		snprintf(lv, sizeof(lv), "V: --");
		snprintf(lp, sizeof(lp), "P: --");
	}
	cfb_print(disp, li, 0, s_fh);
	cfb_print(disp, lv, 0, s_fh * 2);
	cfb_print(disp, lp, 0, s_fh * 3);
}

static void page_rolling(const struct device *disp)
{
	uint16_t win = config_get()->stats_window_s;
	char primary[12]; snprintf(primary, sizeof(primary), "ROLL %us", (unsigned)win);
	render_status_bar(disp, primary);

	char lines[3][16], buf[12];
	const enum stats_ch chs[3] = { STATS_CH_SLOW_I, STATS_CH_SLOW_V, STATS_CH_SLOW_P };
	for (int i = 0; i < 3; i++) {
		struct stats_window w;
		if (stats_window_get(chs[i], win, &w)) {
			fmt_si_f(buf, sizeof(buf), w.mean, ch_unit(chs[i]));
			snprintf(lines[i], sizeof(lines[i]), "%s: %s", ch_label(chs[i]), buf);
		} else {
			snprintf(lines[i], sizeof(lines[i]), "%s: --", ch_label(chs[i]));
		}
	}
	cfb_print(disp, lines[0], 0, s_fh);
	cfb_print(disp, lines[1], 0, s_fh * 2);
	cfb_print(disp, lines[2], 0, s_fh * 3);
}

static void page_peaks(const struct device *disp)
{
	uint16_t win = config_get()->stats_window_s;
	uint8_t cycle = config_get()->stats_window_s ? 4 : 4;   /* placeholder; cfg.ui_cycle_s in Phase 4 */
	enum stats_ch ch = cycle_channel(cycle);

	char primary[12]; snprintf(primary, sizeof(primary), "PEAK %s %us", ch_label(ch), (unsigned)win);
	render_status_bar(disp, primary);

	struct stats_window w;
	if (!stats_window_get(ch, win, &w)) {
		cfb_print(disp, "  (no data)", 0, s_fh * 2);
		return;
	}
	char b[12], line[16];
	fmt_si_f(b, sizeof(b), w.min,  ch_unit(ch)); snprintf(line, sizeof(line), "min %s", b); cfb_print(disp, line, 0, s_fh);
	fmt_si_f(b, sizeof(b), w.mean, ch_unit(ch)); snprintf(line, sizeof(line), "avg %s", b); cfb_print(disp, line, 0, s_fh * 2);
	fmt_si_f(b, sizeof(b), w.max,  ch_unit(ch)); snprintf(line, sizeof(line), "max %s", b); cfb_print(disp, line, 0, s_fh * 3);
}

static void page_hold(const struct device *disp)
{
	uint8_t cycle = 4;
	enum stats_ch ch = cycle_channel(cycle);

	char primary[12]; snprintf(primary, sizeof(primary), "HOLD %s", ch_label(ch));
	render_status_bar(disp, primary);

	struct stats_hold h;
	if (!stats_hold_get(ch, &h)) {
		cfb_print(disp, "  (no data)", 0, s_fh * 2);
		return;
	}
	int64_t age = k_uptime_get() - h.since_uptime_ms;
	char b[12], line[16];
	fmt_si_f(b, sizeof(b), h.min, ch_unit(ch)); snprintf(line, sizeof(line), "min %s", b); cfb_print(disp, line, 0, s_fh);
	fmt_si_f(b, sizeof(b), h.max, ch_unit(ch)); snprintf(line, sizeof(line), "max %s", b); cfb_print(disp, line, 0, s_fh * 2);
	snprintf(line, sizeof(line), "for %lld s", (long long)(age / 1000));
	cfb_print(disp, line, 0, s_fh * 3);
}

static void page_energy(const struct device *disp)
{
	render_status_bar(disp, "ENRGY");

	double wh = stats_energy_wh();
	double q_as = stats_energy_q_as();
	int64_t ms = stats_energy_elapsed_ms();

	char le[16], lq[16], lt[16];
	/* E: mWh if < 1 Wh, else Wh. */
	if (wh < 1.0) snprintf(le, sizeof(le), "E %.2f mWh", wh * 1000.0);
	else          snprintf(le, sizeof(le), "E %.3f Wh",  wh);
	/* Q: mAh always (compact). */
	snprintf(lq, sizeof(lq), "Q %.2f mAh", q_as * 1000.0 / 3600.0);
	int sec = (int)(ms / 1000);
	snprintf(lt, sizeof(lt), "t %d:%02d:%02d", sec / 3600, (sec / 60) % 60, sec % 60);

	cfb_print(disp, le, 0, s_fh);
	cfb_print(disp, lq, 0, s_fh * 2);
	cfb_print(disp, lt, 0, s_fh * 3);
}

static void page_file(const struct device *disp)
{
	render_status_bar(disp, "FILE");

	const char *path = storage_session_path();
	const char *name = path ? strrchr(path, '/') : NULL;
	char shortname[16];
	if (name && *(name + 1)) {
		strncpy(shortname, name + 1, sizeof(shortname) - 1);
		shortname[sizeof(shortname) - 1] = '\0';
	} else {
		snprintf(shortname, sizeof(shortname), "(none)");
	}
	char l1[16], l2[16];
	snprintf(l1, sizeof(l1), "%s", shortname);
	snprintf(l2, sizeof(l2), "drp %u", (unsigned)acq_drops());
	cfb_print(disp, l1, 0, s_fh);
	cfb_print(disp, l2, 0, s_fh * 2);
	/* Third line: TODO -- free SD space (needs an fs_statvfs helper). */
}

static void page_system(const struct device *disp)
{
	render_status_bar(disp, "SYS");

	int64_t up_ms = k_uptime_get();
	int up_s = (int)(up_ms / 1000);
	int mv = power_batt_mv();

	char lu[16], lb[16], lv[16];
	snprintf(lu, sizeof(lu), "up %d:%02d:%02d", up_s / 3600, (up_s / 60) % 60, up_s % 60);
	if (power_usb_present() && mv <= 0) {
		snprintf(lb, sizeof(lb), "src USB");
	} else if (mv > 0) {
		snprintf(lb, sizeof(lb), "bat %d.%02dV", mv / 1000, (mv % 1000) / 10);
	} else {
		snprintf(lb, sizeof(lb), "bat --");
	}
	/* Reuse INA228 die temp as a rough system temp proxy until power.h grows
	 * one. (The real SoC die isn't read; this is the nearest-onboard reading.) */
	struct stats_hold ht;
	if (stats_hold_get(STATS_CH_SLOW_T, &ht)) {
		snprintf(lv, sizeof(lv), "T %.1f C", (double)ht.max);
	} else {
		snprintf(lv, sizeof(lv), "T --");
	}
	cfb_print(disp, lu, 0, s_fh);
	cfb_print(disp, lb, 0, s_fh * 2);
	cfb_print(disp, lv, 0, s_fh * 3);
}

/* ---- top-level render dispatch ----------------------------------------- */
void ui_render(const struct device *disp)
{
	cfb_framebuffer_clear(disp, false);

	switch (page_get()) {
	case PAGE_LIVE:   page_live(disp);    break;
	case PAGE_ROLL:   page_rolling(disp); break;
	case PAGE_PEAKS:  page_peaks(disp);   break;
	case PAGE_HOLD:   page_hold(disp);    break;
	case PAGE_ENERGY: page_energy(disp);  break;
	case PAGE_FILE:   page_file(disp);    break;
	case PAGE_SYS:    page_system(disp);  break;
	default:          page_live(disp);    break;
	}

	cfb_framebuffer_finalize(disp);
}

#else  /* !CONFIG_APP_DISPLAY -- stubs so callers don't need #ifdef */

void ui_init(void) {}
void ui_render(const struct device *disp) { (void)disp; }
void ui_next_page(void) {}
void ui_prev_page(void) {}
void ui_set_page(enum ui_page p) { (void)p; }
void ui_kick(void) {}
void ui_hold_clear_current(void) {}
enum ui_page ui_get_page(void) { return PAGE_LIVE; }

#endif
