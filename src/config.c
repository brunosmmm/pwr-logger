#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(config, LOG_LEVEL_INF);

#define CFG_VERSION 4
#define CFG_KEY     "pwr/cfg"
#define CFG_BLOB    CFG_KEY "/blob"

static struct app_cfg g_cfg;

enum cfg_type { T_BOOL, T_U8, T_U16, T_U32 };

struct cfg_item {
	const char   *key;
	enum cfg_type type;
	size_t        off;
	uint32_t      def;
};

#define ITEM(k, t, field, d) { k, t, offsetof(struct app_cfg, field), d }

static const struct cfg_item items[] = {
	ITEM("autostart",       T_BOOL, autostart,       1),
	ITEM("autooff_en",      T_BOOL, autooff_en,      0),
	ITEM("autooff_s",       T_U32,  autooff_s,       1800),
	ITEM("idle_ble",        T_BOOL, idle_ble,        0),
	ITEM("pof_trip_mv",     T_U16,  pof_trip_mv,     3200),
	ITEM("pof_crit_mv",     T_U16,  pof_crit_mv,     3000),
	ITEM("usb_unplug",      T_U8,   usb_unplug,      USB_UNPLUG_RESUME),
	ITEM("ts_require_sync", T_BOOL, ts_require_sync, 0),
	ITEM("file_fmt",        T_U8,   file_fmt,        FILE_FMT_UTC),
	ITEM("adv_idle_ms",     T_U16,  adv_idle_ms,     1000),
	ITEM("cal_enable",      T_BOOL, cal_enable,      1),
	ITEM("cal_tau_s",       T_U16,  cal_tau_s,       20),
	ITEM("stats_window_s",  T_U16,  stats_window_s,  10),
};

static void write_field(const struct cfg_item *it, uint32_t v)
{
	void *p = (uint8_t *)&g_cfg + it->off;

	switch (it->type) {
	case T_BOOL: *(bool *)p = (bool)v;        break;
	case T_U8:   *(uint8_t *)p = (uint8_t)v;  break;
	case T_U16:  *(uint16_t *)p = (uint16_t)v; break;
	case T_U32:  *(uint32_t *)p = v;          break;
	}
}

static uint32_t read_field(const struct cfg_item *it)
{
	void *p = (uint8_t *)&g_cfg + it->off;

	switch (it->type) {
	case T_BOOL: return *(bool *)p;
	case T_U8:   return *(uint8_t *)p;
	case T_U16:  return *(uint16_t *)p;
	case T_U32:  return *(uint32_t *)p;
	}
	return 0;
}

void config_set_defaults(void)
{
	memset(&g_cfg, 0, sizeof(g_cfg));
	g_cfg.version = CFG_VERSION;
	for (size_t i = 0; i < ARRAY_SIZE(items); i++) {
		write_field(&items[i], items[i].def);
	}
}

const struct app_cfg *config_get(void) { return &g_cfg; }

static const struct cfg_item *find(const char *key)
{
	for (size_t i = 0; i < ARRAY_SIZE(items); i++) {
		if (strcmp(items[i].key, key) == 0) {
			return &items[i];
		}
	}
	return NULL;
}

int config_save(void)
{
	return settings_save_one(CFG_BLOB, &g_cfg, sizeof(g_cfg));
}

int config_set(const char *key, const char *valstr)
{
	const struct cfg_item *it = find(key);
	uint32_t v;

	if (!it) {
		return -ENOENT;
	}
	if (it->type == T_BOOL) {
		if (!strcmp(valstr, "on") || !strcmp(valstr, "true") || !strcmp(valstr, "1")) {
			v = 1;
		} else if (!strcmp(valstr, "off") || !strcmp(valstr, "false") ||
			   !strcmp(valstr, "0")) {
			v = 0;
		} else {
			return -EINVAL;
		}
	} else {
		char *end;

		v = strtoul(valstr, &end, 0);
		if (*end != '\0') {
			return -EINVAL;
		}
	}
	write_field(it, v);
	return config_save();
}

int config_get_str(const char *key, char *buf, size_t len)
{
	const struct cfg_item *it = find(key);

	if (!it) {
		return -ENOENT;
	}
	snprintf(buf, len, "%u", (unsigned)read_field(it));
	return 0;
}

void config_foreach(config_cb cb, void *ctx)
{
	char buf[16];

	for (size_t i = 0; i < ARRAY_SIZE(items); i++) {
		config_get_str(items[i].key, buf, sizeof(buf));
		cb(items[i].key, buf, ctx);
	}
}

static int cfg_h_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	struct app_cfg tmp;

	if (settings_name_steq(name, "blob", &next) && !next) {
		if (len == sizeof(tmp) &&
		    read_cb(cb_arg, &tmp, sizeof(tmp)) == (ssize_t)sizeof(tmp) &&
		    tmp.version == CFG_VERSION) {
			g_cfg = tmp;
		} else {
			LOG_WRN("stored cfg incompatible -> keeping defaults");
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(pwr_cfg, CFG_KEY, NULL, cfg_h_set, NULL, NULL);

int config_load(void)
{
	int rc;

	config_set_defaults();
	rc = settings_subsys_init();
	if (rc) {
		LOG_ERR("settings_subsys_init -> %d", rc);
		return rc;
	}
	/* Loads every static handler, including the wall-clock's. */
	settings_load();
	return 0;
}
