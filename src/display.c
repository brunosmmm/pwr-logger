#include "display.h"

#if defined(CONFIG_APP_DISPLAY)

#include "ina228.h"
#include "logctl.h"
#include "power.h"
#include "clock.h"
#include "acquisition.h"
#include "app_config.h"

#include <stdio.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

/* INA228 LSBs (match ina228.c). */
#define INA228_CURRENT_LSB (INA228_IMAX_A / 524288.0f)

/* These 0.96" yellow-blue OLEDs have a physical color split at row 16: the
 * top 16 px is yellow phosphor, the rest blue. The controller treats both
 * regions identically (same GDDRAM, monochrome) -- whatever we draw at
 * y=[0..15] just APPEARS yellow. So we use those 16 px as a fixed-format
 * status bar (state + a rotating context field) and y=[16..63] as the data
 * area (I / V / P at 10x16). */

/* 3-char state abbreviations so the status-bar format is width-stable. */
static const char *state_abbr3(int s)
{
	switch (s) {
	case 1: return "REC";  /* LOGGING */
	case 2: return "OFL";  /* OFFLOAD */
	case 3: return "SHP";  /* SHIP    */
	case 4: return "IDL";  /* IDLE    */
	default: return "STP"; /* STOPPED */
	}
}

/* Format a µ-unit value (µA / µW) into "<int>.<2frac> <prefix><unit>", picking
 * unit-prefix automatically by magnitude. Integer math, no FP-printf. */
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

/* Format a milli-unit value (mV) similarly: "X.XX V" or "<int> mV". */
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

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

K_THREAD_STACK_DEFINE(disp_stack, 1536);
static struct k_thread disp_thread;
static uint8_t s_fh = 16;   /* font height (px), refined at init */

/* Status-bar field rotation (yellow strip @ y=0). Available fields depend on
 * runtime: time slot only joins the cycle once the clock is synced (otherwise
 * a "no time" placeholder would waste a slot forever). 4 renders @ 500 ms =
 * 2 s per field, so cycle length is 4 s (no time) or 6 s (synced). */
enum sbar_field { F_PWR, F_TIME, F_DROPS };

static void render_status_bar(char *bar, size_t sz)
{
	static uint8_t tick;
	static uint8_t cyc;

	if (++tick >= 4) {
		tick = 0;
		cyc++;
	}

	enum sbar_field fs[3];
	uint8_t nf = 0;
	fs[nf++] = F_PWR;
	if (clk_is_synced()) {
		fs[nf++] = F_TIME;
	}
	fs[nf++] = F_DROPS;

	const char *st = state_abbr3(logctl_state());

	switch (fs[cyc % nf]) {
	case F_PWR: {
		/* On USB the "battery" SAADC reading is just the USB-fed VDDH
		 * rail -- meaningless to the operator -- so show only "USB". On
		 * battery, show the voltage (state of charge). */
		bool usb = power_usb_present();
		int mv = power_batt_mv();
		if (usb) {
			snprintf(bar, sz, "%-3s USB", st);
		} else if (mv > 0) {
			snprintf(bar, sz, "%-3s BAT %d.%dV",
				 st, mv / 1000, (mv % 1000) / 100);
		} else {
			snprintf(bar, sz, "%-3s BAT --", st);
		}
		break;
	}
	case F_TIME: {
		time_t t = clk_now();
		snprintf(bar, sz, "%-3s %02d:%02d:%02d", st,
			 (int)((t / 3600) % 24),
			 (int)((t / 60) % 60),
			 (int)(t % 60));
		break;
	}
	case F_DROPS: {
		uint32_t d = acq_drops();
		if (d > 9999u) {
			snprintf(bar, sz, "%-3s drp 9999+", st);
		} else {
			snprintf(bar, sz, "%-3s drp %u", st, d);
		}
		break;
	}
	}
}

static void render(void)
{
	char bar[16], li[16], lv[16], lp[16];
	struct ina228_sample s;

	render_status_bar(bar, sizeof(bar));

	if (ina228_last(&s)) {
		int i_ua = (int)((float)s.current_raw * INA228_CURRENT_LSB * 1e6f);
		int v_mv = (int)((float)s.vbus_raw * 0.1953125f);
		int p_uw = (int)((float)s.power_raw * 3.2f * INA228_CURRENT_LSB * 1e6f);
		char buf[12];

		fmt_micro(buf, sizeof(buf), i_ua, 'A'); snprintf(li, sizeof(li), "I: %s", buf);
		fmt_milli(buf, sizeof(buf), v_mv, 'V'); snprintf(lv, sizeof(lv), "V: %s", buf);
		fmt_micro(buf, sizeof(buf), p_uw, 'W'); snprintf(lp, sizeof(lp), "P: %s", buf);
	} else {
		snprintf(li, sizeof(li), "I: --");
		snprintf(lv, sizeof(lv), "V: --");
		snprintf(lp, sizeof(lp), "P: --");
	}

	cfb_framebuffer_clear(disp, false);
	cfb_print(disp, bar, 0, 0);           /* yellow strip */
	cfb_print(disp, li,  0, s_fh);        /* y=16 */
	cfb_print(disp, lv,  0, s_fh * 2);    /* y=32 */
	cfb_print(disp, lp,  0, s_fh * 3);    /* y=48 */
	cfb_framebuffer_finalize(disp);
}

/* When the 3V3 rail cycles in dark IDLE, the SSD1306 controller loses ALL
 * its config (no battery-backed registers). The render thread must stop
 * touching the chip while it's powered down, and the chip must be fully
 * re-initialized on wake. `s_powered` gates the render thread; the driver's
 * own init function is re-run via dev->ops.init() on wake. */
static atomic_t s_powered = ATOMIC_INIT(1);

/* Transient overlay shown by the render thread in place of the normal
 * I/V/P layout (e.g. "powering off in 3 s"). Active when `s_notice_until_ms`
 * is in the future (k_uptime_get_32() reference). */
static uint32_t s_notice_until_ms;
static char     s_notice_msg[20];

void display_notice(const char *msg, uint32_t duration_ms)
{
	if (duration_ms == 0 || msg == NULL) {
		s_notice_until_ms = 0;
		return;
	}
	strncpy(s_notice_msg, msg, sizeof(s_notice_msg) - 1);
	s_notice_msg[sizeof(s_notice_msg) - 1] = '\0';
	s_notice_until_ms = k_uptime_get_32() + duration_ms;
}

static bool render_notice_if_active(void)
{
	uint32_t now = k_uptime_get_32();
	int32_t remaining = (int32_t)(s_notice_until_ms - now);
	if (s_notice_until_ms == 0 || remaining <= 0) {
		return false;
	}
	cfb_framebuffer_clear(disp, false);
	cfb_print(disp, s_notice_msg, 0, s_fh);   /* y=16, in the blue area */
	cfb_framebuffer_finalize(disp);
	return true;
}

static void disp_loop(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (1) {
		if (atomic_get(&s_powered)) {
			if (!render_notice_if_active()) {
				render();
			}
		}
		k_sleep(K_MSEC(500));   /* ~2 Hz: gentle on the bus + CPU */
	}
}

void display_set_powered(bool on)
{
	if (on) {
		/* SSD1306 power-on settling. The driver's own init uses
		 * K_TIMEOUT_ABS_MS for the ready-time wait, which is in the
		 * past on a runtime re-init -- so the first I2C write would
		 * land before the chip finishes its internal POR. 100 ms is
		 * well beyond the datasheet's ~10 ms requirement. */
		k_sleep(K_MSEC(100));

		/* Re-run the SSD1306 driver's init function -- re-sends the
		 * full config sequence to a chip that lost everything when
		 * the rail cycled. dev->ops.init is a public field; we skip
		 * device_init() (it returns -EALREADY for an already-init
		 * device). */
		if (disp->ops.init) {
			int rc = disp->ops.init(disp);
			LOG_INF("display: re-init rc=%d", rc);
			if (rc) {
				return;
			}
		}
		display_blanking_off(disp);
		cfb_framebuffer_clear(disp, true);
		atomic_set(&s_powered, 1);
		display_notice(NULL, 0);   /* clear any stale notice */
		LOG_INF("display: powered up");
	} else {
		atomic_set(&s_powered, 0);
		LOG_INF("display: powered down (rail cut imminent)");
	}
}

void display_init(void)
{
	uint8_t fw;

	if (!device_is_ready(disp)) {
		LOG_ERR("SSD1306 not ready");
		return;
	}
	if (cfb_framebuffer_init(disp) != 0) {
		LOG_ERR("cfb init failed");
		return;
	}
	display_blanking_off(disp);
	cfb_framebuffer_set_font(disp, 0);
	if (cfb_get_font_size(disp, 0, &fw, &s_fh) != 0 || s_fh == 0) {
		s_fh = 16;
	}
	cfb_framebuffer_clear(disp, true);

	k_thread_create(&disp_thread, disp_stack, K_THREAD_STACK_SIZEOF(disp_stack),
			disp_loop, NULL, NULL, NULL, 8, 0, K_NO_WAIT);
	k_thread_name_set(&disp_thread, "display");
	LOG_INF("SSD1306 display up (font %ux%u)", fw, s_fh);
}

#endif /* CONFIG_APP_DISPLAY */
