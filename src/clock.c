#include "clock.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(wclock, LOG_LEVEL_INF);

#define CLK_KEY   "pwr/clk"
#define CLK_EPOCH CLK_KEY "/epoch"

/* wall_ms = s_base_ms + k_uptime_get() */
static int64_t s_base_ms;
static bool    s_has_time;
static bool    s_synced;
static int64_t s_loaded_epoch = -1;   /* from settings during load, -1 = none */

void clk_init(void)
{
	s_base_ms = 0;
	s_has_time = false;
	s_synced = false;
	s_loaded_epoch = -1;
}

time_t clk_now(void)
{
	if (!s_has_time) {
		return 0;
	}
	return (time_t)((s_base_ms + k_uptime_get()) / 1000);
}

bool clk_is_synced(void) { return s_synced; }
bool clk_has_time(void)  { return s_has_time; }

static void persist(void)
{
	int64_t now;

	if (!s_has_time) {
		return;
	}
	now = (int64_t)clk_now();
	settings_save_one(CLK_EPOCH, &now, sizeof(now));
}

void clk_persist(void) { persist(); }

void clk_set(time_t t)
{
	s_base_ms = (int64_t)t * 1000 - k_uptime_get();
	s_has_time = true;
	s_synced = true;
	persist();
	LOG_INF("clock synced -> %lld", (long long)t);
}

static void persist_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(persist_work, persist_work_fn);

/* Persist cadence. Bounds how stale the restored epoch is on an
 * unplanned reset (panic, watchdog, power loss). 60 s keeps the worst-
 * case loss at one minute, costing ~1 NVS write/min = ~1440/day. The
 * settings NVS subsystem wear-levels across multiple sectors so this
 * is comfortable on a 10k-cycle/page flash budget. Planned shutdown
 * paths (pwr reboot, OTA apply, sys_reboot) call clk_persist() inline
 * so they lose nothing. */
#define CLK_PERSIST_PERIOD K_SECONDS(60)

static void persist_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	persist();
	k_work_schedule(&persist_work, CLK_PERSIST_PERIOD);
}

void clk_start(void)
{
	if (s_loaded_epoch >= 0) {
		s_base_ms = s_loaded_epoch * 1000 - k_uptime_get();
		s_has_time = true;
		s_synced = false;   /* restored estimate, not a true sync */
		LOG_INF("clock restored ~%lld (estimate, awaiting sync)",
			(long long)s_loaded_epoch);
	} else {
		LOG_INF("clock unset (awaiting sync)");
	}
	k_work_schedule(&persist_work, CLK_PERSIST_PERIOD);
}

static int clk_h_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	int64_t v;

	if (settings_name_steq(name, "epoch", &next) && !next) {
		if (len == sizeof(v) &&
		    read_cb(cb_arg, &v, sizeof(v)) == (ssize_t)sizeof(v)) {
			s_loaded_epoch = v;
		}
		return 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(pwr_clk, CLK_KEY, NULL, clk_h_set, NULL, NULL);
