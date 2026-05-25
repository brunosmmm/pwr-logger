#include "logctl.h"
#include "logger.h"
#include "storage.h"
#include "config.h"
#include "clock.h"
#include "power.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_POWEROFF)
#include <zephyr/sys/poweroff.h>
#endif

LOG_MODULE_REGISTER(logctl, LOG_LEVEL_INF);

#define EVQ_LEN 8

K_MSGQ_DEFINE(evq, sizeof(uint8_t), EVQ_LEN, 1);

static enum logctl_state s_state = LS_STOPPED;

static const char *const names[] = { "stopped", "logging", "offload", "ship" };

const char *logctl_state_name(enum logctl_state s) { return names[s]; }
enum logctl_state logctl_state(void) { return s_state; }

int logctl_post(enum logctl_event ev)
{
	uint8_t e = (uint8_t)ev;

	return k_msgq_put(&evq, &e, K_NO_WAIT);
}

/* ---- transitions ------------------------------------------------------- */
static void to_logging(void)
{
	if (s_state == LS_LOGGING) {
		return;
	}
	/* logger_start() mounts the card + opens a fresh session + starts acq. */
	logger_start();
	s_state = LS_LOGGING;
	LOG_INF("-> LOGGING");
}

static void to_stopped(void)
{
	if (s_state == LS_LOGGING) {
		logger_stop();
	} else if (s_state == LS_OFFLOAD) {
		(void)storage_mount();   /* re-take the card we released */
	}
	s_state = LS_STOPPED;
	power_batt_sample();             /* SAADC free now */
	LOG_INF("-> STOPPED");
}

static void to_offload(void)
{
	if (s_state == LS_OFFLOAD) {
		return;
	}
	if (s_state == LS_LOGGING) {
		logger_stop();           /* flush + close the file, stop acq */
	}
	(void)storage_unmount();         /* release FatFs so the host owns the blocks */
	s_state = LS_OFFLOAD;
	power_batt_sample();             /* SAADC free now */
	LOG_INF("-> OFFLOAD (SD released; host may mount the drive)");
}

static void to_ship(void)
{
	if (s_state == LS_LOGGING) {
		logger_stop();
	}
	clk_persist();
	LOG_INF("-> SHIP (powering off)");
#if defined(CONFIG_POWEROFF)
	k_sleep(K_MSEC(50));             /* let logs/CDC drain */
	sys_poweroff();                  /* does not return */
#endif
	s_state = LS_SHIP;
}

static void handle(enum logctl_event ev)
{
	switch (ev) {
	case EV_REC_TOGGLE:
		(s_state == LS_LOGGING) ? to_stopped() : to_logging();
		break;
	case EV_LOG_START:
		to_logging();
		break;
	case EV_LOG_STOP:
		to_stopped();
		break;
	case EV_ROTATE:
		if (s_state == LS_LOGGING) {
			logger_rotate();
		}
		break;
	case EV_OFFLOAD_ON:
		to_offload();
		break;
	case EV_OFFLOAD_OFF:
		to_logging();            /* resume logging on offload exit */
		break;
	case EV_POWEROFF:
		to_ship();
		break;
	case EV_LOW_BATT:
		/* protect the recording now; deeper power policy in power.c */
		if (s_state == LS_LOGGING) {
			to_stopped();
		}
		break;
	case EV_USB_ON:
	case EV_USB_OFF:
	case EV_AUTOOFF:
		/* wired with power management (Step 4) */
		break;
	}
}

static void logctl_thread(void *a, void *b, void *c)
{
	uint8_t e;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	while (1) {
		if (k_msgq_get(&evq, &e, K_FOREVER) == 0) {
			handle((enum logctl_event)e);
		}
	}
}

/* Stack must cover logger_start()'s chain: FatFs mount + storage_session_begin
 * (gmtime_r/snprintf) + tdms_open. 2048 overflowed; match main's 4096. */
K_THREAD_DEFINE(logctl_tid, 4096, logctl_thread, NULL, NULL, NULL, 7, 0, 0);

void logctl_init(void)
{
	power_batt_sample();   /* SAADC free before logging starts */
	if (config_get()->autostart) {
		logctl_post(EV_LOG_START);
	} else {
		LOG_INF("autostart off -> STOPPED (use 'pwr log' / REC)");
	}
}
