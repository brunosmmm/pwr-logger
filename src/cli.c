#include "config.h"
#include "clock.h"

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
	shell_print(sh, "time:   %s (synced=%d)", b, clk_is_synced());
	shell_print(sh, "uptime: %lld ms", k_uptime_get());
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

SHELL_STATIC_SUBCMD_SET_CREATE(time_sub,
	SHELL_CMD(get, NULL, "show current UTC time", cmd_time_get),
	SHELL_CMD(set, NULL, "set time: pwr time set <epoch>", cmd_time_set),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(cfg_sub,
	SHELL_CMD(get,  NULL, "get <key>", cmd_cfg_get),
	SHELL_CMD(set,  NULL, "set <key> <value>", cmd_cfg_set),
	SHELL_CMD(list, NULL, "list all config", cmd_cfg_list),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(pwr_sub,
	SHELL_CMD(status, NULL, "device status", cmd_status),
	SHELL_CMD(time,   &time_sub, "time get/set", NULL),
	SHELL_CMD(config, &cfg_sub,  "config get/set/list", NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pwr, &pwr_sub, "pwr-logger control", NULL);
