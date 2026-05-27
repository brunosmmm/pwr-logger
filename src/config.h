#ifndef CONFIG_STORE_H_
#define CONFIG_STORE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Persistent configuration (Zephyr settings over NVS in the "storage" flash
 * partition). One packed struct, stored as a single versioned blob; a string
 * key/type table drives the console/BLE get/set/list front-ends.
 */

enum usb_unplug_action { USB_UNPLUG_RESUME = 0, USB_UNPLUG_IDLE = 1 };
enum file_fmt_kind     { FILE_FMT_UTC = 0,      FILE_FMT_INDEX = 1 };

struct app_cfg {
	uint8_t  version;
	bool     autostart;        /* begin logging automatically at boot            */
	bool     autooff_en;       /* dark-idle inactivity -> System OFF             */
	uint32_t autooff_s;        /* inactivity timeout (s) before auto-off         */
	bool     idle_ble;         /* keep BLE advertising in idle (connectable idle)*/
	uint16_t pof_trip_mv;      /* POFCON low-battery clean-shutdown threshold     */
	uint16_t pof_crit_mv;      /* below this -> straight to System OFF            */
	uint8_t  usb_unplug;       /* enum usb_unplug_action after offload            */
	bool     ts_require_sync;  /* refuse timestamped session until time synced    */
	uint8_t  file_fmt;         /* enum file_fmt_kind                              */
	uint16_t adv_idle_ms;      /* BLE advertising interval in connectable idle    */
	bool     cal_enable;       /* auto-zero the fast channel against the INA228   */
	uint16_t cal_tau_s;        /* offset-tracking time constant (s)               */
	uint16_t stats_window_s;   /* rolling-stats window length (s), 1..60          */
	/* UI / multi-page state */
	uint8_t  ui_default_page;  /* boot default page (0..6 = LIVE..SYSTEM)         */
	uint16_t ui_timeout_s;     /* auto-return to default after idle s; 0 = never  */
	uint8_t  ui_cycle_s;       /* PEAKS/HOLD channel auto-cycle period (s)        */
	/* Stats engine policies */
	bool     stats_reset_on_start; /* clear rolling buffers on logger_start       */
	bool     hold_reset_on_start;  /* clear HOLD latches on logger_start          */
	bool     energy_reset_on_start;/* clear energy accumulators on logger_start   */
	/* Display sleep */
	uint16_t display_sleep_s;  /* blank panel after N s idle; 0 = always on       */
	/* SD config file (Phase 5) */
	bool     cfgfile_load_on_boot; /* read /SD:/pwr.cfg into NVS on boot          */
	/* Reserved: T_F32 example, also used by future alert thresholds */
	float    alerts_i_max_a;   /* fast-channel current limit (A), |I| over -> notify */
};

typedef void (*config_cb)(const char *key, const char *val, void *ctx);

int  config_load(void);                 /* defaults + settings_subsys_init + load */
void config_set_defaults(void);
const struct app_cfg *config_get(void);
int  config_set(const char *key, const char *valstr);  /* parse + apply + save   */
int  config_get_str(const char *key, char *buf, size_t len);
void config_foreach(config_cb cb, void *ctx);
int  config_save(void);

#endif /* CONFIG_STORE_H_ */
