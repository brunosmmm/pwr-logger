#include "usb_msc.h"
#include "storage.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pwr_usb, LOG_LEVEL_INF);

/*
 * The MSC disk is bound to the SD card via CONFIG_MASS_STORAGE_DISK_NAME="SD".
 * Logging (FatFs) and MSC (host owns the block device) must never run at once
 * (REQUIREMENTS.md section 6) — switch modes explicitly.
 */

int usb_storage_init(void)
{
	/* TODO: usb_enable(NULL). Keep MSC offline / not-ready in logging mode. */
	LOG_WRN("usb_storage_init: TODO usb_enable + mode gating");
	return 0;
}

int usb_offload_enter(void)
{
	/* TODO: logger_stop(); storage_unmount(); present the card as MSC. */
	LOG_INF("usb_offload_enter (stub)");
	return 0;
}

int usb_offload_exit(void)
{
	/* TODO: hide MSC; storage_mount(); resume logging if desired. */
	LOG_INF("usb_offload_exit (stub)");
	return 0;
}
