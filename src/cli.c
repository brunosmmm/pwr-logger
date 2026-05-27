#include "config.h"
#include "clock.h"
#include "logctl.h"
#include "power.h"
#include "cal.h"
#include "stats.h"
#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

/*
 * Console front-end (Zephyr shell over the CDC-ACM USB console). Commands are
 * thin wrappers over config/clock (and later logctl). Root command: `pwr`.
 */

static void fmt_utc(time_t t, char *buf, size_t len)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	char b[32];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (clk_has_time()) {
		fmt_utc(clk_now(), b, sizeof(b));
	} else {
		strcpy(b, "(unset)");
	}
	shell_print(sh, "state:  %s", logctl_state_name(logctl_state()));
	shell_print(sh, "time:   %s (synced=%d)", b, clk_is_synced());
	shell_print(sh, "uptime: %lld ms", k_uptime_get());
	shell_print(sh, "fast cal offset: %d uA", (int)(cal_offset_a() * 1e6f));
	return 0;
}

static int cmd_log(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_LOG_START);
	shell_print(sh, "logging");
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_LOG_STOP);
	shell_print(sh, "stopped");
	return 0;
}

static int cmd_rotate(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_ROTATE);
	shell_print(sh, "rotate requested");
	return 0;
}

static int cmd_offload(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_OFFLOAD_ON);
	shell_print(sh, "offload: stopping + releasing SD; mount the USB drive, then 'pwr resume'");
	return 0;
}

static int cmd_resume(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_OFFLOAD_OFF);
	shell_print(sh, "resuming logging (unmount the USB drive on the host first)");
	return 0;
}

static int cmd_poweroff(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_POWEROFF);
	shell_print(sh, "powering off");
	return 0;
}

static int cmd_lowbatt(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_LOW_BATT);
	shell_print(sh, "simulated low-battery event");
	return 0;
}

static int cmd_usb(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: pwr usb <on|off>");
		return -EINVAL;
	}
	if (!strcmp(argv[1], "off")) {
		logctl_post(EV_USB_OFF);
		shell_print(sh, "simulated VBUS removed");
	} else {
		logctl_post(EV_USB_ON);
		shell_print(sh, "simulated VBUS ready");
	}
	return 0;
}

static int cmd_batt(const struct shell *sh, size_t argc, char **argv)
{
	int mv;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (logctl_state() != LS_LOGGING) {
		power_batt_sample();   /* SAADC free -> fresh reading */
	}
	mv = power_batt_mv();
	if (mv < 0) {
		shell_print(sh, "battery: unknown (not sampled yet)");
	} else {
		shell_print(sh, "battery (VDDH): %d.%03d V%s", mv / 1000, mv % 1000,
			    logctl_state() == LS_LOGGING ? "  (cached; SAADC busy logging)" : "");
	}
	return 0;
}

static int cmd_time_get(const struct shell *sh, size_t argc, char **argv)
{
	char b[32];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	fmt_utc(clk_now(), b, sizeof(b));
	shell_print(sh, "%lld  %s  synced=%d", (long long)clk_now(), b, clk_is_synced());
	return 0;
}

static int cmd_time_set(const struct shell *sh, size_t argc, char **argv)
{
	time_t t;
	char b[32];

	if (argc < 2) {
		shell_error(sh, "usage: pwr time set <unix-epoch-seconds>");
		return -EINVAL;
	}
	t = (time_t)strtoll(argv[1], NULL, 0);
	clk_set(t);
	fmt_utc(t, b, sizeof(b));
	shell_print(sh, "set %s", b);
	return 0;
}

static int cmd_cfg_get(const struct shell *sh, size_t argc, char **argv)
{
	char v[16];

	if (argc < 2) {
		shell_error(sh, "usage: pwr config get <key>");
		return -EINVAL;
	}
	if (config_get_str(argv[1], v, sizeof(v))) {
		shell_error(sh, "no such key '%s'", argv[1]);
		return -ENOENT;
	}
	shell_print(sh, "%s = %s", argv[1], v);
	return 0;
}

static int cmd_cfg_set(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	if (argc < 3) {
		shell_error(sh, "usage: pwr config set <key> <value>");
		return -EINVAL;
	}
	rc = config_set(argv[1], argv[2]);
	if (rc) {
		shell_error(sh, "set failed (%d)", rc);
		return rc;
	}
	shell_print(sh, "ok");
	return 0;
}

static void list_cb(const char *key, const char *val, void *ctx)
{
	shell_print((const struct shell *)ctx, "  %-16s = %s", key, val);
}

static int cmd_cfg_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	config_foreach(list_cb, (void *)sh);
	return 0;
}

static const char *const ch_names[STATS_CH__COUNT] = {
	"fast_i", "slow_i", "slow_v", "slow_p", "slow_t",
};
static const char *const ch_units[STATS_CH__COUNT] = {
	"A", "A", "V", "W", "C",
};

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	uint16_t win = config_get()->stats_window_s;
	shell_print(sh, "rolling window: %u s", (unsigned)win);
	shell_print(sh, "  channel    mean        min         max         n");
	for (int i = 0; i < STATS_CH__COUNT; i++) {
		struct stats_window w;
		if (!stats_window_get((enum stats_ch)i, win, &w)) {
			shell_print(sh, "  %-8s   (no data)", ch_names[i]);
			continue;
		}
		shell_print(sh, "  %-8s  % .4g %s   % .4g %s   % .4g %s   %u",
			    ch_names[i], (double)w.mean, ch_units[i],
			    (double)w.min, ch_units[i],
			    (double)w.max, ch_units[i], (unsigned)w.n);
	}
	return 0;
}

static int cmd_hold(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (int i = 0; i < STATS_CH__COUNT; i++) {
		struct stats_hold h;
		if (!stats_hold_get((enum stats_ch)i, &h)) {
			shell_print(sh, "  %-8s   (no data)", ch_names[i]);
			continue;
		}
		int64_t age = k_uptime_get() - h.since_uptime_ms;
		shell_print(sh, "  %-8s  min % .4g %s  max % .4g %s  for %lld ms",
			    ch_names[i], (double)h.min, ch_units[i],
			    (double)h.max, ch_units[i], (long long)age);
	}
	return 0;
}

static int cmd_hold_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	stats_hold_clear(STATS_CH_ALL);
	shell_print(sh, "hold cleared (all channels)");
	return 0;
}

static int cmd_energy(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	double wh = stats_energy_wh();
	double q  = stats_energy_q_as();
	int64_t ms = stats_energy_elapsed_ms();
	shell_print(sh, "  E  = %.4g mWh   ( %.6g Wh )",
		    wh * 1000.0, wh);
	shell_print(sh, "  Q  = %.4g mAh   ( %.6g As )",
		    q * 1000.0 / 3600.0, q);
	shell_print(sh, "  t  = %lld ms",  (long long)ms);
	return 0;
}

static int cmd_energy_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	stats_energy_clear();
	shell_print(sh, "energy accumulator reset");
	return 0;
}

/* `pwr ui page N` -- jump to a specific UI page (0..PAGE__COUNT-1).
 * Mostly diagnostic; the operator usually uses ROTATE button (Phase 3). */
static const char *const page_names[PAGE__COUNT] = {
	"LIVE", "ROLLING", "PEAKS", "HOLD", "ENERGY", "FILE", "SYSTEM",
};

static int cmd_ui_page(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "current page: %d (%s)",
			    (int)ui_get_page(), page_names[ui_get_page()]);
		shell_print(sh, "available:");
		for (int i = 0; i < PAGE__COUNT; i++) {
			shell_print(sh, "  %d  %s", i, page_names[i]);
		}
		return 0;
	}
	int p = atoi(argv[1]);
	if (p < 0 || p >= PAGE__COUNT) {
		shell_error(sh, "page out of range (0..%d)", PAGE__COUNT - 1);
		return -EINVAL;
	}
	ui_set_page((enum ui_page)p);
	shell_print(sh, "page = %d (%s)", p, page_names[p]);
	return 0;
}

static int cmd_ui_next(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	ui_next_page();
	shell_print(sh, "page = %d (%s)", (int)ui_get_page(), page_names[ui_get_page()]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(time_sub,
	SHELL_CMD(get, NULL, "show current UTC time", cmd_time_get),
	SHELL_CMD(set, NULL, "set time: pwr time set <epoch>", cmd_time_set),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(cfg_sub,
	SHELL_CMD(get,  NULL, "get <key>", cmd_cfg_get),
	SHELL_CMD(set,  NULL, "set <key> <value>", cmd_cfg_set),
	SHELL_CMD(list, NULL, "list all config", cmd_cfg_list),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(hold_sub,
	SHELL_CMD(show,  NULL, "print HOLD latches",   cmd_hold),
	SHELL_CMD(clear, NULL, "clear all HOLD",       cmd_hold_clear),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(energy_sub,
	SHELL_CMD(show,  NULL, "print accumulators",   cmd_energy),
	SHELL_CMD(clear, NULL, "reset accumulators",   cmd_energy_clear),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(ui_sub,
	SHELL_CMD(page, NULL, "page [N]: show or set page", cmd_ui_page),
	SHELL_CMD(next, NULL, "advance to next page",       cmd_ui_next),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(pwr_sub,
	SHELL_CMD(status,   NULL, "device status", cmd_status),
	SHELL_CMD(log,      NULL, "start logging", cmd_log),
	SHELL_CMD(stop,     NULL, "stop logging", cmd_stop),
	SHELL_CMD(rotate,   NULL, "cut a new session file", cmd_rotate),
	SHELL_CMD(offload,  NULL, "stop + expose SD as USB drive", cmd_offload),
	SHELL_CMD(resume,   NULL, "leave offload, resume logging", cmd_resume),
	SHELL_CMD(poweroff, NULL, "clean stop + power off", cmd_poweroff),
	SHELL_CMD(batt,     NULL, "battery (VDDH) voltage", cmd_batt),
	SHELL_CMD(lowbatt,  NULL, "simulate low-battery event (test)", cmd_lowbatt),
	SHELL_CMD(usb,      NULL, "simulate VBUS on/off (test)", cmd_usb),
	SHELL_CMD(time,     &time_sub,   "time get/set", NULL),
	SHELL_CMD(config,   &cfg_sub,    "config get/set/list", NULL),
	SHELL_CMD(stats,    NULL,        "rolling min/mean/max per channel", cmd_stats),
	SHELL_CMD(hold,     &hold_sub,   "HOLD show/clear", NULL),
	SHELL_CMD(energy,   &energy_sub, "energy accumulators", NULL),
	SHELL_CMD(ui,       &ui_sub,     "UI page select", NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pwr, &pwr_sub, "pwr-logger control", NULL);
