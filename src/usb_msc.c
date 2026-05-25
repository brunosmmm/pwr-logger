#include "usb_msc.h"
#include "storage.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pwr_usb, LOG_LEVEL_INF);

/*
 * Composite USB device: CDC-ACM (console) + Mass Storage (SD-card offload).
 * We own the single device_next context here instead of the board's standalone
 * cdc_acm_serial console context (disabled via
 * CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n), so MSC can share the device.
 *
 * MSC gives the host raw block access to the SD card, so FatFs logging and MSC
 * are mutually exclusive (REQUIREMENTS.md section 6): unmount FatFs before the
 * host touches the card. The mode switch is driven over the CDC console.
 */

USBD_DEVICE_DEFINE(pwr_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   0x2fe3 /* Nordic test VID */, 0x0006 /* pwr-logger */);

USBD_DESC_LANG_DEFINE(pwr_lang);
USBD_DESC_MANUFACTURER_DEFINE(pwr_mfr, "pwr-logger");
USBD_DESC_PRODUCT_DEFINE(pwr_product, "pwr-logger");
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(pwr_sn)));

USBD_DESC_CONFIG_DEFINE(pwr_fs_cfg_desc, "pwr-logger FS");
USBD_CONFIGURATION_DEFINE(pwr_fs_config, USB_SCD_SELF_POWERED, 250, &pwr_fs_cfg_desc);

/* MSC LUN bound to the SD disk (disk-name "SD" from the devicetree). */
USBD_DEFINE_MSC_LUN(sd, "SD", "pwrlog", "SD card", "0.10");

int usb_storage_init(void)
{
	int err;

	err = usbd_add_descriptor(&pwr_usbd, &pwr_lang);
	if (!err) {
		err = usbd_add_descriptor(&pwr_usbd, &pwr_mfr);
	}
	if (!err) {
		err = usbd_add_descriptor(&pwr_usbd, &pwr_product);
	}
	IF_ENABLED(CONFIG_HWINFO, (
		if (!err) {
			err = usbd_add_descriptor(&pwr_usbd, &pwr_sn);
		}
	))
	if (err) {
		LOG_ERR("usbd add descriptor -> %d", err);
		return err;
	}

	err = usbd_add_configuration(&pwr_usbd, USBD_SPEED_FS, &pwr_fs_config);
	if (err) {
		LOG_ERR("usbd add configuration -> %d", err);
		return err;
	}

	err = usbd_register_all_classes(&pwr_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		LOG_ERR("usbd register classes -> %d", err);
		return err;
	}

	/* CDC-ACM spans two interfaces -> advertise an IAD via the MISC triple. */
	usbd_device_set_code_triple(&pwr_usbd, USBD_SPEED_FS,
				    USB_BCC_MISCELLANEOUS, 0x02, 0x01);

	/* MSC queries the disk but never initializes it; bring up the SD card so
	 * the host sees real capacity (FatFs would otherwise do this on mount). */
	err = disk_access_init("SD");
	if (err) {
		LOG_ERR("disk_access_init(SD) -> %d (MSC will show no media)", err);
	}

	err = usbd_init(&pwr_usbd);
	if (err) {
		LOG_ERR("usbd_init -> %d", err);
		return err;
	}

	err = usbd_enable(&pwr_usbd);
	if (err) {
		LOG_ERR("usbd_enable -> %d", err);
		return err;
	}

	LOG_INF("USB up: CDC console + MSC (SD card)");
	return 0;
}

int usb_offload_enter(void)
{
	/* TODO (increment 2): logger_stop(); storage_unmount(); so the host can
	 * safely own the block device. For now logging is not started. */
	LOG_INF("usb_offload_enter (stub)");
	return 0;
}

int usb_offload_exit(void)
{
	/* TODO (increment 2): storage_mount(); resume logging. */
	LOG_INF("usb_offload_exit (stub)");
	return 0;
}
