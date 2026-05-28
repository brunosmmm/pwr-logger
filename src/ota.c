/*
 * SD-card OTA: drop /SD:/firmware.bin, swap to it on next reset.
 *
 * Pipeline:
 *   1. mount SD if needed
 *   2. stat /SD:/firmware.bin -- exists? larger than the MCUboot
 *      header? fits in the secondary slot?
 *   3. flash_img_init for the secondary slot, stream the file into it
 *      with flash_img_buffered_write. Feed the WDT inline -- the full
 *      ~250 KB write at SPI flash erase-write speed (~10-30 ms/page)
 *      easily exceeds the 4 s watchdog window.
 *   4. boot_request_upgrade(BOOT_UPGRADE_TEST) -- MCUboot will swap the
 *      slot on next reset; if the new image doesn't call
 *      boot_write_img_confirmed during init, MCUboot reverts.
 *   5. rename firmware.bin -> firmware.bin.applied so we don't re-apply
 *      forever.
 *   6. sys_reboot.
 *
 * Confirm-on-good-boot is the symmetric half: main() calls
 * ota_confirm_current() at the end of init once everything has come up.
 * If we panic / wedge / fail during early init, MCUboot reverts on the
 * next reset and we're back on the prior firmware automatically. No
 * brick risk from a bad SD payload as long as the prior image was
 * confirmed.
 */

#include <errno.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include "ota.h"
#include "storage.h"
#include "wdt.h"
#include "display.h"

/* Secondary slot size from devicetree (no runtime overhead). */
#define OTA_SLOT_SIZE  FIXED_PARTITION_SIZE(slot1_partition)

/* OLED display indicator. NVMC-stall freezes the display thread for tens of
 * milliseconds per flash page so the panel updates in steps; users see
 * "OTA  5%" -> "OTA 10%" etc. Long display_notice timeout (30 s) means each
 * percent re-call refreshes the same overlay; the value lingers across the
 * stalls. */
static void ota_show(const char *msg)
{
	display_notice(msg, 30000);
}

static void ota_show_pct(size_t done, size_t total)
{
	char buf[20];
	int pct = (total > 0) ? (int)((done * 100U) / total) : 0;

	(void)snprintf(buf, sizeof(buf), "OTA %3d%%", pct);
	ota_show(buf);
}

/* Small read buffer so we don't add to the already-tight BSS footprint.
 * flash_img_buffered_write accumulates into its own page-sized buffer
 * internally, so the read chunk size mainly affects loop overhead. */
#define OTA_CHUNK  1024
static uint8_t ota_buf[OTA_CHUNK];

int ota_apply_from_sd(ota_progress_cb_t cb, void *user)
{
#define REPORT(stage, val)  do { if (cb) cb(stage, (val), user); } while (0)

	ota_show("OTA start");

	bool we_mounted = (storage_mount() == 0);

	struct fs_dirent ent;
	int rc = fs_stat(OTA_FILE_PATH, &ent);

	REPORT("stat", rc);
	if (rc != 0 || ent.type != FS_DIR_ENTRY_FILE) {
		if (we_mounted) {
			(void)storage_unmount();
		}
		return (rc != 0) ? rc : -ENOENT;
	}

	/* Smallest sane image: MCUboot TLV header is 32 B; anything below
	 * that can't be a real signed app. */
	if (ent.size < 32) {
		if (we_mounted) {
			(void)storage_unmount();
		}
		return -EINVAL;
	}

	if ((size_t)ent.size > OTA_SLOT_SIZE) {
		if (we_mounted) {
			(void)storage_unmount();
		}
		return -E2BIG;
	}

	struct fs_file_t fp;

	fs_file_t_init(&fp);
	rc = fs_open(&fp, OTA_FILE_PATH, FS_O_READ);
	REPORT("fs_open", rc);
	if (rc != 0) {
		if (we_mounted) {
			(void)storage_unmount();
		}
		return rc;
	}

	struct flash_img_context ctx;

	rc = flash_img_init(&ctx);
	REPORT("flash_img_init", rc);
	if (rc != 0) {
		(void)fs_close(&fp);
		if (we_mounted) {
			(void)storage_unmount();
		}
		return rc;
	}

	size_t total = (size_t)ent.size;
	size_t remaining = total;
	size_t last_shown_pct = 0;

	ota_show_pct(0, total);

	while (remaining > 0) {
		size_t want = remaining > OTA_CHUNK ? OTA_CHUNK : remaining;
		ssize_t n = fs_read(&fp, ota_buf, want);

		if (n <= 0) {
			REPORT("fs_read", (n == 0) ? -EIO : (int)n);
			(void)fs_close(&fp);
			if (we_mounted) {
				(void)storage_unmount();
			}
			return (n == 0) ? -EIO : (int)n;
		}

		bool last = (remaining == (size_t)n);

		rc = flash_img_buffered_write(&ctx, ota_buf, (size_t)n, last);
		if (rc != 0) {
			REPORT("flash_write", rc);
			(void)fs_close(&fp);
			if (we_mounted) {
				(void)storage_unmount();
			}
			return rc;
		}

		remaining -= (size_t)n;

		/* Update the OLED every ~5% so the user sees progress
		 * advancing. snprintf into the static buf is cheap; the
		 * notice's own lock protects the underlying string. */
		size_t done = total - remaining;
		size_t pct = (done * 100U) / total;

		if (pct >= last_shown_pct + 5 || remaining == 0) {
			ota_show_pct(done, total);
			last_shown_pct = pct;
		}

		/* Long synchronous write loop; sysworkq feed has stopped
		 * making progress. Keep the WDT alive inline. */
		wdt_feed_now();
	}
	REPORT("stream", 0);

	(void)fs_close(&fp);

	/* Mark for TEST swap on next reset. boot_request_upgrade(0) =
	 * TEST (revert-on-no-confirm); (1) = PERMANENT. Always go via
	 * TEST so a broken payload reverts on its own. */
	rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
	REPORT("mark", rc);
	if (rc != 0) {
		if (we_mounted) {
			(void)storage_unmount();
		}
		return rc;
	}

	/* Don't re-apply the same image on subsequent boots. */
	(void)fs_rename(OTA_FILE_PATH, OTA_APPLIED_PATH);
	REPORT("rename", 0);

	ota_show("OTA reboot");

	if (we_mounted) {
		(void)storage_unmount();
	}

	/* Cold reset so MCUboot picks up the pending TEST swap. */
	sys_reboot(SYS_REBOOT_COLD);

	/* unreachable */
	return 0;
}

void ota_confirm_current(void)
{
	if (!boot_is_img_confirmed()) {
		(void)boot_write_img_confirmed();
	}
}

bool ota_test_pending(void)
{
	return !boot_is_img_confirmed();
}

int ota_pending_swap_type(void)
{
	return mcuboot_swap_type();
}
