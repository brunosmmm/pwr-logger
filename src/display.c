#include "display.h"

#if defined(CONFIG_APP_DISPLAY)

#include "ina228.h"
#include "logctl.h"
#include "power.h"
#include "app_config.h"

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(display, LOG_LEVEL_INF);

/* INA228 LSBs (match ina228.c). */
#define INA228_CURRENT_LSB (INA228_IMAX_A / 524288.0f)

static const struct device *const disp = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

K_THREAD_STACK_DEFINE(disp_stack, 1536);
static struct k_thread disp_thread;
static uint8_t s_fh = 16;   /* font height (px), refined at init */

static void render(void)
{
	char l[4][22];
	struct ina228_sample s;
	int batt = power_batt_mv();

	if (ina228_last(&s)) {
		/* integer formatting -> no float-printf dependency */
		int i_ua = (int)((float)s.current_raw * INA228_CURRENT_LSB * 1e6f);
		int v_mv = (int)((float)s.vbus_raw * 0.1953125f);
		int p_uw = (int)((float)s.power_raw * 3.2f * INA228_CURRENT_LSB * 1e6f);

		snprintf(l[0], sizeof(l[0]), "I:%d uA", i_ua);
		snprintf(l[1], sizeof(l[1]), "V:%d mV", v_mv);
		snprintf(l[2], sizeof(l[2]), "P:%d uW", p_uw);
	} else {
		snprintf(l[0], sizeof(l[0]), "I: --");
		snprintf(l[1], sizeof(l[1]), "V: --");
		snprintf(l[2], sizeof(l[2]), "P: --");
	}
	snprintf(l[3], sizeof(l[3]), "%s %d.%02dV",
		 logctl_state_name(logctl_state()),
		 batt > 0 ? batt / 1000 : 0, batt > 0 ? (batt % 1000) / 10 : 0);

	cfb_framebuffer_clear(disp, false);
	for (int i = 0; i < 4; i++) {
		cfb_print(disp, l[i], 0, i * s_fh);
	}
	cfb_framebuffer_finalize(disp);
}

static void disp_loop(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (1) {
		render();
		k_sleep(K_MSEC(500));   /* ~2 Hz: gentle on the bus + CPU */
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
