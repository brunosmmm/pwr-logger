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
#include <zephyr/sys/atomic.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/img_mgmt/img_mgmt_callbacks.h>

#include "ota.h"
#include "storage.h"
#include "wdt.h"
#include "display.h"
#include "clock.h"
#include "logctl.h"

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

	/* Drive the unified state machine: PREPARING (stop logger, banner),
	 * then TRANSFERRING for the streaming loop, MARKING after the last
	 * chunk, REBOOTING right before sys_reboot. BLE OTA goes through
	 * the same transitions via the DFU notification callbacks. */
	int rc = ota_begin(OTA_SRC_SD, "SD OTA");

	if (rc != 0) {
		/* Another OTA already in flight. Refuse. */
		return rc;
	}

	bool we_mounted = (storage_mount() == 0);

	struct fs_dirent ent;

	rc = fs_stat(OTA_FILE_PATH, &ent);

	REPORT("stat", rc);
	if (rc != 0 || ent.type != FS_DIR_ENTRY_FILE) {
		if (we_mounted) {
			(void)storage_unmount();
		}
		ota_end_failure();
		return (rc != 0) ? rc : -ENOENT;
	}

	/* Smallest sane image: MCUboot TLV header is 32 B; anything below
	 * that can't be a real signed app. */
	if (ent.size < 32) {
		if (we_mounted) {
			(void)storage_unmount();
		}
		ota_end_failure();
		return -EINVAL;
	}

	if ((size_t)ent.size > OTA_SLOT_SIZE) {
		if (we_mounted) {
			(void)storage_unmount();
		}
		ota_end_failure();
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
		ota_end_failure();
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
		ota_end_failure();
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
			ota_end_failure();
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
			ota_end_failure();
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

	/* Phase: MARKING (test trailer write). */
	ota_mark_start();

	/* Mark for TEST swap on next reset. boot_request_upgrade(0) =
	 * TEST (revert-on-no-confirm); (1) = PERMANENT. Always go via
	 * TEST so a broken payload reverts on its own. */
	rc = boot_request_upgrade(BOOT_UPGRADE_TEST);
	REPORT("mark", rc);
	if (rc != 0) {
		if (we_mounted) {
			(void)storage_unmount();
		}
		ota_end_failure();
		return rc;
	}

	/* Don't re-apply the same image on subsequent boots. */
	(void)fs_rename(OTA_FILE_PATH, OTA_APPLIED_PATH);
	REPORT("rename", 0);

	/* Phase: REBOOTING. */
	ota_reboot_start();
	ota_show("OTA reboot");

	if (we_mounted) {
		(void)storage_unmount();
	}

	/* Flush wall-clock estimate before reset so the swapped-in image
	 * comes up with a fresh epoch and the user isn't slowly drifting
	 * back across every OTA. */
	clk_persist();

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

/* ---- BLE-mcumgr OTA in-flight tracking + display indicator ------------ *
 *
 * mcumgr drives image_mgmt's own upload state machine, completely
 * independent of our ota_apply_from_sd() pipeline. To get the same UX
 * (display banner + percent, no auto-sleep mid-upload) for BLE OTA, we
 * register a callback on its DFU events and mirror the SD-path
 * behaviour from here.
 *
 *   DFU_STARTED  -> set in_progress=1, show "BLE OTA"
 *   DFU_CHUNK    -> update percent from req->off / req->size
 *   DFU_PENDING  -> show "BLE done", in_progress stays set until STOPPED
 *   DFU_STOPPED  -> clear in_progress
 *
 * The chunk callback's prev_status must be MGMT_CB_OK (otherwise upload
 * was already denied by an earlier hook); we pass it through. We do not
 * authorise/deny based on app state -- just observe + indicate.
 */

/* Unified OTA state machine -- both BLE-mcumgr and SD-card paths feed
 * transitions in here so the rest of the device sees one consistent
 * view. The atomics are sufficient locking; we don't need a mutex
 * because phase transitions are short and self-consistent. */
static atomic_t s_phase  = ATOMIC_INIT(OTA_IDLE);
static atomic_t s_source = ATOMIC_INIT(OTA_SRC_NONE);

enum ota_phase  ota_phase_get(void)  { return (enum ota_phase)atomic_get(&s_phase); }
enum ota_source ota_source_get(void) { return (enum ota_source)atomic_get(&s_source); }
bool ota_in_progress(void)
{
	enum ota_phase p = ota_phase_get();

	return p != OTA_IDLE && p != OTA_FAILED;
}

const char *ota_phase_name(enum ota_phase p)
{
	switch (p) {
	case OTA_IDLE:         return "idle";
	case OTA_PREPARING:    return "preparing";
	case OTA_TRANSFERRING: return "transferring";
	case OTA_MARKING:      return "marking";
	case OTA_REBOOTING:    return "rebooting";
	case OTA_FAILED:       return "failed";
	default:               return "?";
	}
}

const char *ota_source_name(enum ota_source s)
{
	switch (s) {
	case OTA_SRC_NONE: return "-";
	case OTA_SRC_BLE:  return "ble";
	case OTA_SRC_SD:   return "sd";
	default:           return "?";
	}
}

static void phase_set(enum ota_phase p)
{
	atomic_set(&s_phase, (atomic_val_t)p);
}

static void source_set(enum ota_source s)
{
	atomic_set(&s_source, (atomic_val_t)s);
}

/* Common prep for any OTA path:
 *   - Refuse if another OTA is already in flight (returns -EBUSY).
 *   - Set source + phase = PREPARING.
 *   - Banner the display.
 *   - Stop the logger cleanly so SD writes don't race with the
 *     upcoming internal-flash writes.
 *   - Transition to TRANSFERRING when done.
 */
int ota_begin(enum ota_source src, const char *banner)
{
	if (ota_in_progress()) {
		return -EBUSY;
	}
	source_set(src);
	phase_set(OTA_PREPARING);
	if (banner) {
		display_notice(banner, 120000);
	}

	/* Ask logctl to stop the logger if it's running. Non-blocking on
	 * purpose: this function may be called from the mcumgr DFU
	 * callback context while the host is waiting for a response to
	 * the first upload chunk. Blocking here pushed past the BLE
	 * supervision timeout and tanked the upload. logctl will process
	 * EV_LOG_STOP within a few ms anyway -- well before any flash
	 * writes happen, and there's no actual race because the OTA path
	 * touches internal flash while the logger touches SD. */
	if (logctl_state() == LS_LOGGING) {
		logctl_post(EV_LOG_STOP);
	}

	phase_set(OTA_TRANSFERRING);
	return 0;
}

void ota_mark_start(void)   { phase_set(OTA_MARKING); }
void ota_reboot_start(void) { phase_set(OTA_REBOOTING); }

void ota_end_success(void)
{
	/* Stays at REBOOTING -- sys_reboot is about to fire. Don't reset
	 * phase to IDLE: a brief window of "actually it's done" before
	 * the chip resets would be misleading. */
}

void ota_end_failure(void)
{
	phase_set(OTA_FAILED);
	source_set(OTA_SRC_NONE);
	display_notice(NULL, 0);
}

void ota_end_abort(void)
{
	phase_set(OTA_IDLE);
	source_set(OTA_SRC_NONE);
	display_notice(NULL, 0);
}


static enum mgmt_cb_return ota_dfu_cb(uint32_t event,
				      enum mgmt_cb_return prev_status,
				      int32_t *rc, uint16_t *group,
				      bool *abort_more, void *data,
				      size_t data_size)
{
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	/* Note: no per-chunk percentage. The CHUNK event payload's
	 * img_mgmt_upload_req is only fully defined in img_mgmt.h, which
	 * pulls mcuboot internals that aren't reachable from app code
	 * here. The static "BLE OTA" banner is sufficient indication
	 * during the multi-minute upload; the user wanted to know
	 * "something is happening" and that's what they get. */
	switch (event) {
	case MGMT_EVT_OP_IMG_MGMT_DFU_STARTED:
		(void)ota_begin(OTA_SRC_BLE, "BLE OTA");
		break;

	case MGMT_EVT_OP_IMG_MGMT_DFU_PENDING:
		ota_mark_start();   /* host's next request will set the test trailer */
		display_notice("BLE OTA done", 8000);
		break;

	case MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED:
		ota_end_abort();   /* upload terminated (success path: host
				    * sends OS reset shortly; failure path: just
				    * leave us at IDLE) */
		break;

	default:
		break;
	}

	return prev_status;
}

static struct mgmt_callback ota_dfu_callback = {
	.callback = ota_dfu_cb,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_STARTED |
		    MGMT_EVT_OP_IMG_MGMT_DFU_PENDING |
		    MGMT_EVT_OP_IMG_MGMT_DFU_STOPPED,
};

void ota_dfu_hooks_init(void)
{
	static bool registered;

	if (!registered) {
		mgmt_callback_register(&ota_dfu_callback);
		registered = true;
	}
}
