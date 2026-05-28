#include "display.h"

#if defined(CONFIG_APP_DISPLAY)

#include "ui.h"
#include "config.h"
#include "power.h"
#include "logctl.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

/*
 * Display owns the SSD1306 device handle, the render thread lifecycle, the
 * notice-overlay mechanism (cross-thread-safe), and the rail-cycle re-init
 * dance. All UI composition (status bar, per-page renderers, format helpers)
 * lives in src/ui.c; this module's render() is now a thin dispatcher.
 */

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

K_THREAD_STACK_DEFINE(disp_stack, 1536);
static struct k_thread disp_thread;
static uint8_t s_fh = 16;   /* font height (px), refined at init -- for the notice overlay */

/* Render is now a thin dispatcher to ui_render(). All composition (status bar,
 * per-page renderers, format helpers) lives in src/ui.c. */
static void render(void)
{
	ui_render(disp);
}

/* When the 3V3 rail cycles in dark IDLE, the SSD1306 controller loses ALL
 * its config (no battery-backed registers). The render thread must stop
 * touching the chip while it's powered down, and the chip must be fully
 * re-initialized on wake. `s_powered` gates the render thread; the driver's
 * own init function is re-run via dev->ops.init() on wake. */
static atomic_t s_powered = ATOMIC_INIT(1);

/* Transient overlay shown by the render thread in place of the normal
 * I/V/P layout (e.g. "powering off"). Writers (logctl, system workqueue
 * via controls) and reader (display thread) all touch these fields, so
 * accesses are serialized by `s_notice_lock`. Without the lock the
 * reader could pass cfb_print() a half-rewritten string -- one of its
 * mid-rewrite bytes can fall outside the font's char range, producing a
 * huge index in the glyph table -> faulting load -> the display thread
 * gets silently aborted by the fault handler while everything else
 * keeps running. (Observed once after ~15 min of stop/offload/resume
 * cycles; see the parallel-agent root-cause analysis in commit msg.) */
static struct k_mutex s_notice_lock;
static uint32_t       s_notice_until_ms;
static char           s_notice_msg[20];

void display_notice(const char *msg, uint32_t duration_ms)
{
	k_mutex_lock(&s_notice_lock, K_FOREVER);
	if (duration_ms == 0 || msg == NULL) {
		s_notice_until_ms = 0;
	} else {
		strncpy(s_notice_msg, msg, sizeof(s_notice_msg) - 1);
		s_notice_msg[sizeof(s_notice_msg) - 1] = '\0';
		s_notice_until_ms = k_uptime_get_32() + duration_ms;
	}
	k_mutex_unlock(&s_notice_lock);
}

static bool render_notice_if_active(void)
{
	char     local_msg[sizeof(s_notice_msg)];
	uint32_t until;

	k_mutex_lock(&s_notice_lock, K_FOREVER);
	until = s_notice_until_ms;
	memcpy(local_msg, s_notice_msg, sizeof(local_msg));
	k_mutex_unlock(&s_notice_lock);

	local_msg[sizeof(local_msg) - 1] = '\0';   /* belt-and-braces */
	int32_t remaining = (int32_t)(until - k_uptime_get_32());
	if (until == 0 || remaining <= 0) {
		return false;
	}
	cfb_framebuffer_clear(disp, false);
	cfb_print(disp, local_msg, 0, s_fh);
	cfb_framebuffer_finalize(disp);
	return true;
}

/* Idle-blank state. When BATTERY-POWERED and ui_idle_ms() exceeds
 * cfg.display_sleep_s * 1000, the panel goes into blanking (SSD1306 AEh
 * sleep-display command); first tick after a button press unblanks and
 * resumes rendering. On USB power there is no point blanking -- the host
 * is feeding unlimited current -- so the feature is forcibly disabled.
 * Distinct from display_set_powered(): the chip keeps its rail + GDDRAM
 * during blanking; only the on-segment driver is suspended. */
static bool s_blanked;

static void disp_loop(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (1) {
		if (atomic_get(&s_powered)) {
			uint16_t sleep_s = config_get()->display_sleep_s;
			bool on_usb = power_usb_present();
			/* Never blank during LOGGING -- the on-screen state
			 * IS the recording confirmation; an unintended OLED
			 * sleep reads as "the device powered off mid-record"
			 * even though the rail and the writer are still
			 * running. (Observed regression: cfg.display_sleep_s
			 * was non-zero from a prior NVS write, USB unplug
			 * left only LOGGING + battery, ui_idle_ms expired,
			 * panel blanked. User couldn't tell logging was
			 * still going.) */
			bool active = (logctl_state() == LS_LOGGING) ||
				      (logctl_state() == LS_OFFLOAD);
			bool should_blank = !on_usb && (sleep_s > 0) && !active &&
					    (ui_idle_ms() > (int64_t)sleep_s * 1000);

			if (should_blank && !s_blanked) {
				display_blanking_on(disp);
				s_blanked = true;
				LOG_INF("display: blanked (idle %us, on battery)", sleep_s);
			} else if (!should_blank && s_blanked) {
				display_blanking_off(disp);
				s_blanked = false;
				LOG_INF("display: woke");
			}

			if (!s_blanked) {
				if (!render_notice_if_active()) {
					render();
				}
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
	k_mutex_init(&s_notice_lock);
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
