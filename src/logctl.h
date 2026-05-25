#ifndef LOGCTL_H_
#define LOGCTL_H_

/*
 * Mode state machine — the single owner of the device mode. Buttons/switch, the
 * console shell, BLE, USB-VBUS and the battery monitor all post events; logctl
 * serializes them on its own thread and drives logger_*, storage_*, power_* and
 * sys_poweroff. Front-ends never change mode directly.
 */

enum logctl_state {
	LS_STOPPED = 0,   /* not recording (idle/monitor; refined with power mgmt) */
	LS_LOGGING,       /* recording to SD                                       */
	LS_OFFLOAD,       /* FatFs released; SD served to the USB host as MSC      */
	LS_SHIP,          /* powering off                                          */
};

enum logctl_event {
	EV_REC_TOGGLE = 0, /* start<->stop logging                                 */
	EV_LOG_START,
	EV_LOG_STOP,
	EV_ROTATE,         /* cut a new session file, keep recording               */
	EV_OFFLOAD_ON,     /* enter USB-MSC offload                                 */
	EV_OFFLOAD_OFF,    /* leave offload, resume logging                         */
	EV_POWEROFF,       /* clean stop + System OFF                               */
	EV_USB_ON,         /* VBUS present       (handled with power mgmt)          */
	EV_USB_OFF,        /* VBUS removed                                          */
	EV_LOW_BATT,       /* POFCON trip: flush + safe                             */
	EV_AUTOOFF,        /* idle inactivity expired                              */
};

void logctl_init(void);                  /* seed initial mode (after config_load) */
int  logctl_post(enum logctl_event ev);  /* ISR-safe; queues an event             */
enum logctl_state logctl_state(void);
const char *logctl_state_name(enum logctl_state s);

#endif /* LOGCTL_H_ */
