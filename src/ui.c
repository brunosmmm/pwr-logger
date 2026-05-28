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
#include "logger.h"
#include "ble_stream.h"
#include "display.h"

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
	/* Always boot to LIVE regardless of NVS. cfg.ui_default_page still drives
	 * the optional auto-return-on-idle target (cfg.ui_timeout_s, default 0).
	 * Booting straight into PEAKS/HOLD/FILE/etc. surprised operators after a
	 * power cycle and could land them on a page whose backing data isn't
	 * ready yet (e.g. FILE before storage_mount). LIVE is always renderable. */
	s_page = PAGE_LIVE;
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

int64_t ui_idle_ms(void)
{
	int64_t last;
	k_mutex_lock(&s_lock, K_FOREVER);
	last = s_last_input_ms;
	k_mutex_unlock(&s_lock);
	return k_uptime_get() - last;
}

/* Notice overlay containing all eight icon glyphs ('\x40'..'\x47'). The render
 * thread will draw the notice via display_notice's existing path -- using the
 * TEXT font. That's wrong for icons, so instead we steal the page-render slot
 * for `duration_ms` by setting a flag the render thread checks; for now, just
 * use display_notice with a string of icons and rely on the notice render
 * picking up the icons font when the chars are in the 0x40..0x47 range.
 *
 * Implementation: stuff the 8 icon codepoints into a string and trigger a
 * notice -- but the notice renderer uses the text font, so we route through
 * a one-shot path that switches to the icons font before drawing. */
static atomic_t s_icon_test_until_ms;

bool ui_icon_test_active(void)
{
	int32_t remaining = (int32_t)((uint32_t)atomic_get(&s_icon_test_until_ms) -
				      k_uptime_get_32());
	return remaining > 0;
}

void ui_icon_test(uint32_t duration_ms)
{
	atomic_set(&s_icon_test_until_ms,
		   (atomic_val_t)(k_uptime_get_32() + duration_ms));
}

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

/* ---- status bar (yellow strip @ y=0..15, inverted) ---------------------- *
 * Layout (128 x 16) -- three fixed icon slots:
 *   x= 0..15   power source: chain-link icon when on USB, battery icon
 *              (5 fill levels by SoC bucket) when on battery. They're
 *              mutually exclusive so they share the same slot; on USB the
 *              battery reading is meaningless (SAADC reads VBUS via the
 *              divider, ~4.8 V), so no point drawing an empty shell.
 *   x=16..31   REC dot when LS_LOGGING -- its own dedicated slot so it
 *              stays visible even while plugged in.
 *   x=32..47   Bluetooth when a central is subscribed.
 *   x=48..127  primary label text (~8 chars at 10x16).
 *
 * Icon glyph codepoints live in icons_font.c:
 *   '\x40'..'\x44'  battery 0..100 %% in 25 %% steps
 *   '\x45'          chain-link (USB-connected indicator)
 *   '\x46'          Bluetooth
 *   '\x47'          REC dot
 */
static void render_status_bar(const struct device *disp, const char *primary)
{
	bool usb = power_usb_present();
	int pct = power_batt_pct();
	enum logctl_state lst = logctl_state();
	char one[2] = { 0, 0 };

	cfb_framebuffer_set_font(disp, display_font_icons());

	if (ui_icon_test_active()) {
		/* DIAG path: draw all 8 glyphs in a single row across the bar
		 * at 16 px each (= 128 px). Skips the conditional logic so
		 * the user can visually check each bitmap regardless of state. */
		cfb_print(disp, "\x40\x41\x42\x43\x44\x45\x46\x47", 0, 0);
		cfb_invert_area(disp, 0, 0, 128, 16);
		return;
	}

	/* Slot 0: link icon when on USB; battery icon when on battery. */
	if (usb) {
		one[0] = '\x45';
		cfb_print(disp, one, 0, 0);
	} else if (pct >= 0) {
		/* Bucket SoC: <12 = empty, 12..37 = 25%, 37..62 = 50%,
		 * 62..87 = 75%, >=87 = full. */
		uint8_t b = (pct >= 87) ? 4 :
			    (pct >= 62) ? 3 :
			    (pct >= 37) ? 2 :
			    (pct >= 12) ? 1 : 0;
		one[0] = (char)(0x40 + b);
		cfb_print(disp, one, 0, 0);
	}

	/* Slot 1: REC dot when logging (own slot so it's visible on USB too).
	 * Blink at 1 Hz (500 ms on / 500 ms off) so the operator reads it as
	 * "live recording" even without color. The render loop ticks every
	 * 500 ms so each on/off phase covers one frame. */
	if (lst == LS_LOGGING && ((k_uptime_get_32() / 500U) & 1U) == 0U) {
		one[0] = '\x47';
		cfb_print(disp, one, 16, 0);
	}

	/* Slot 2: Bluetooth when a central is subscribed. */
	if (ble_stream_subscribed()) {
		one[0] = '\x46';
		cfb_print(disp, one, 32, 0);
	}

	/* --- primary text right of the icons --------------------------- */
	cfb_framebuffer_set_font(disp, display_font_text());
	if (primary && *primary) {
		cfb_print(disp, primary, 48, 0);
	}

	/* Invert the entire yellow strip -> bright background, dark icons + text. */
	cfb_invert_area(disp, 0, 0, 128, 16);
}

/* ---- per-page renderers (assume framebuffer already cleared) ------------ */
static void page_live(const struct device *disp)
{
	render_status_bar(disp, NULL);

	struct ina228_sample s;
	char li[24], lv[24], lp[24], buf[20];
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
	char primary[20]; snprintf(primary, sizeof(primary), "ROLL %us", (unsigned)win);
	render_status_bar(disp, primary);

	char lines[3][24], buf[20];
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
	uint8_t cycle = config_get()->ui_cycle_s ? config_get()->ui_cycle_s : 4;
	enum stats_ch ch = cycle_channel(cycle);

	char primary[20]; snprintf(primary, sizeof(primary), "PEAK %s %us", ch_label(ch), (unsigned)win);
	render_status_bar(disp, primary);

	struct stats_window w;
	if (!stats_window_get(ch, win, &w)) {
		cfb_print(disp, "  (no data)", 0, s_fh * 2);
		return;
	}
	char b[20], line[24];
	fmt_si_f(b, sizeof(b), w.min,  ch_unit(ch)); snprintf(line, sizeof(line), "min %s", b); cfb_print(disp, line, 0, s_fh);
	fmt_si_f(b, sizeof(b), w.mean, ch_unit(ch)); snprintf(line, sizeof(line), "avg %s", b); cfb_print(disp, line, 0, s_fh * 2);
	fmt_si_f(b, sizeof(b), w.max,  ch_unit(ch)); snprintf(line, sizeof(line), "max %s", b); cfb_print(disp, line, 0, s_fh * 3);
}

static void page_hold(const struct device *disp)
{
	uint8_t cycle = config_get()->ui_cycle_s ? config_get()->ui_cycle_s : 4;
	enum stats_ch ch = cycle_channel(cycle);

	char primary[20]; snprintf(primary, sizeof(primary), "HOLD %s", ch_label(ch));
	render_status_bar(disp, primary);

	struct stats_hold h;
	if (!stats_hold_get(ch, &h)) {
		cfb_print(disp, "  (no data)", 0, s_fh * 2);
		return;
	}
	int64_t age = k_uptime_get() - h.since_uptime_ms;
	char b[16], line[24];
	fmt_si_f(b, sizeof(b), h.min, ch_unit(ch)); snprintf(line, sizeof(line), "min %s", b); cfb_print(disp, line, 0, s_fh);
	fmt_si_f(b, sizeof(b), h.max, ch_unit(ch)); snprintf(line, sizeof(line), "max %s", b); cfb_print(disp, line, 0, s_fh * 2);
	uint32_t age_s = (age > 0) ? (uint32_t)(age / 1000) : 0u;
	snprintf(line, sizeof(line), "for %u s", age_s);
	cfb_print(disp, line, 0, s_fh * 3);
}

static void page_energy(const struct device *disp)
{
	render_status_bar(disp, "ENRGY");

	double wh = stats_energy_wh();
	double q_as = stats_energy_q_as();
	int64_t ms = stats_energy_elapsed_ms();

	char le[24], lq[24], lt[24];
	/* E: mWh if < 1 Wh, else Wh. */
	if (wh < 1.0) snprintf(le, sizeof(le), "E %.2f mWh", wh * 1000.0);
	else          snprintf(le, sizeof(le), "E %.3f Wh",  wh);
	/* Q: mAh always (compact). */
	snprintf(lq, sizeof(lq), "Q %.2f mAh", q_as * 1000.0 / 3600.0);
	uint32_t sec = (ms > 0) ? (uint32_t)(ms / 1000u) : 0u;
	snprintf(lt, sizeof(lt), "t %u:%02u:%02u", sec / 3600u, (sec / 60u) % 60u, sec % 60u);

	cfb_print(disp, le, 0, s_fh);
	cfb_print(disp, lq, 0, s_fh * 2);
	cfb_print(disp, lt, 0, s_fh * 3);
}

/* Human-compact byte count, fits in ~10 chars: "12.3MB", "428KB", "1.05GB". */
static void fmt_bytes(char *out, size_t sz, uint64_t b)
{
	if (b >= (uint64_t)1024 * 1024 * 1024) {
		snprintf(out, sz, "%.1fGB", (double)b / (1024.0 * 1024.0 * 1024.0));
	} else if (b >= 1024u * 1024u) {
		snprintf(out, sz, "%.1fMB", (double)b / (1024.0 * 1024.0));
	} else if (b >= 1024u) {
		snprintf(out, sz, "%uKB", (unsigned)(b / 1024u));
	} else {
		snprintf(out, sz, "%uB", (unsigned)b);
	}
}

static void page_file(const struct device *disp)
{
	render_status_bar(disp, "FILE");

	const char *path = storage_session_path();
	const char *name = (path && *path) ? strrchr(path, '/') : NULL;
	if (name) {
		name++;          /* skip the leading '/' */
	}

	char l1[16], l2[16], l3[24];
	/* Parse the UTC-formatted timestamp "YYYYMMDDTHHMMSSZ.tdms" the
	 * logger uses for synced sessions. Decoded "YYYY-MM-DD" / "HH:MM:SS Z"
	 * is the useful information in the name (more readable than packed
	 * digits, and what a human reads off a calendar). For the indexed
	 * fallback "logNNNN.tdms" the parse fails -- show the raw name. */
	if (name && strlen(name) >= 16 && name[8] == 'T' && name[15] == 'Z') {
		snprintf(l1, sizeof(l1), "%.4s-%.2s-%.2s",
			 name, name + 4, name + 6);          /* date */
		snprintf(l2, sizeof(l2), "%.2s:%.2s:%.2s Z",
			 name + 9, name + 11, name + 13);    /* time + Z */
	} else if (name && *name) {
		/* Indexed fallback or other: show raw name truncated. */
		strncpy(l1, name, sizeof(l1) - 1);
		l1[sizeof(l1) - 1] = '\0';
		l2[0] = '\0';
	} else {
		snprintf(l1, sizeof(l1), "(no file)");
		l2[0] = '\0';
	}

	/* Cached snapshots populated once per second by the logger writer. */
	uint64_t file_b = 0, free_b = 0;
	logger_sd_usage(&file_b, &free_b, NULL);

	char szbuf[12], freebuf[12];
	if (file_b > 0 && free_b > 0) {
		fmt_bytes(szbuf, sizeof(szbuf), file_b);
		fmt_bytes(freebuf, sizeof(freebuf), free_b);
		snprintf(l3, sizeof(l3), "%s f%s", szbuf, freebuf);
	} else if (file_b > 0) {
		fmt_bytes(szbuf, sizeof(szbuf), file_b);
		snprintf(l3, sizeof(l3), "%s", szbuf);
	} else if (free_b > 0) {
		fmt_bytes(freebuf, sizeof(freebuf), free_b);
		snprintf(l3, sizeof(l3), "free %s", freebuf);
	} else {
		snprintf(l3, sizeof(l3), "(no data)");
	}

	cfb_print(disp, l1, 0, s_fh);
	if (l2[0]) {
		cfb_print(disp, l2, 0, s_fh * 2);
	}
	cfb_print(disp, l3, 0, s_fh * 3);
}

static void page_system(const struct device *disp)
{
	render_status_bar(disp, "SYS");

	int mv = power_batt_mv();
	int pct = power_batt_pct();

	char ld[16], lt[16], lb[24];

	/* Wall clock: the user complained that no page shows current date /
	 * time. SYS is the natural home -- system status answers 'when, what
	 * state'. Two lines for date + time-of-day; the trailing 'Z' on the
	 * time is replaced with '?' when clk_is_synced() is false so the user
	 * can tell at a glance whether the displayed time is authoritative
	 * (set via 'pwr time set ...') or just an estimate restored from NVS. */
	time_t now = clk_now();
	if (clk_has_time() && now > 0) {
		struct tm tm;
		gmtime_r(&now, &tm);
		snprintf(ld, sizeof(ld), "%04d-%02d-%02d",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
		snprintf(lt, sizeof(lt), "%02d:%02d:%02d %c",
			 tm.tm_hour, tm.tm_min, tm.tm_sec,
			 clk_is_synced() ? 'Z' : '?');
	} else {
		snprintf(ld, sizeof(ld), "clock unset");
		snprintf(lt, sizeof(lt), "(no time)");
	}

	/* Power source on line 3. USB takes priority: on USB the SAADC battery
	 * channel reads VDD via the divider (~4.8 V meaningless), so only show
	 * 'bat <V>' when actually running from battery. SoC %% is the primary
	 * readout; raw mV is auxiliary for diagnosis. */
	if (power_usb_present()) {
		snprintf(lb, sizeof(lb), "src USB");
	} else if (pct >= 0) {
		snprintf(lb, sizeof(lb), "bat %d%% %d.%02dV",
			 pct, mv / 1000, (mv % 1000) / 10);
	} else {
		snprintf(lb, sizeof(lb), "bat --");
	}

	cfb_print(disp, ld, 0, s_fh);
	cfb_print(disp, lt, 0, s_fh * 2);
	cfb_print(disp, lb, 0, s_fh * 3);
}

/* ---- top-level render dispatch ----------------------------------------- */
void ui_render(const struct device *disp)
{
	/* Auto-return to the configured default page after `ui_timeout_s`
	 * seconds with no button input. Skip if timeout disabled (0). */
	uint16_t to = config_get()->ui_timeout_s;
	if (to > 0) {
		int64_t last;
		k_mutex_lock(&s_lock, K_FOREVER); last = s_last_input_ms; k_mutex_unlock(&s_lock);
		if ((k_uptime_get() - last) > (int64_t)to * 1000) {
			uint8_t def = config_get()->ui_default_page;
			if (def < PAGE__COUNT && page_get() != (enum ui_page)def) {
				ui_set_page((enum ui_page)def);
			}
		}
	}

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
