#ifndef OTA_H_
#define OTA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Update file lives at this fixed path on the SD. Drop a signed app
 * binary (zephyr.signed.bin from sysbuild) there, then either:
 *   - reboot with config `ota.auto_apply_on_boot=true`, OR
 *   - issue `pwr ota apply` over the shell / BLE.
 * On success the file is renamed to OTA_APPLIED_PATH so the same
 * image isn't re-applied on every boot. */
#define OTA_FILE_PATH       "/SD:/firmware.bin"
#define OTA_APPLIED_PATH    "/SD:/firmware.bin.applied"

/* Optional progress callback: invoked at each stage of the apply
 * pipeline with a short label ("stat", "open", "stream", "mark",
 * "rename", "done") and a status code (0 on success, negative errno on
 * failure of that stage). Pass NULL to skip progress reporting. */
typedef void (*ota_progress_cb_t)(const char *stage, int rc, void *user);

/* Stream OTA_FILE_PATH into the MCUboot secondary slot, mark a TEST
 * upgrade, rename the file, sys_reboot. The new image must call
 * ota_confirm_current() during init or MCUboot will revert on the
 * next reset. Returns negative errno on failure; on success, does
 * not return (the device reboots).
 *
 * Mounts the SD if it isn't already; restores the mount state on
 * any failure path. */
int ota_apply_from_sd(ota_progress_cb_t cb, void *user);

/* Mark the running image as confirmed (sticky across resets). Safe to
 * call always: on a non-test image it's a no-op. Idempotent. */
void ota_confirm_current(void);

/* Returns true if the running image is unconfirmed (just-swapped TEST
 * image awaiting confirmation). */
bool ota_test_pending(void);

/* Returns the MCUboot swap type that will run on next reset
 * (BOOT_SWAP_TYPE_NONE/TEST/PERM/REVERT/FAIL). Useful for `pwr ota
 * status`. */
int ota_pending_swap_type(void);

/* Unified OTA pipeline phases. Both BLE-mcumgr and SD-card update
 * paths feed into the same state machine so the rest of the device
 * (logctl, display, cli) sees one consistent view of "is an OTA
 * happening, what stage, where did it come from".
 *
 *   IDLE         no OTA underway
 *   PREPARING    stopping the logger, getting ready
 *   TRANSFERRING bytes flowing into slot 1
 *   MARKING      upload done, writing the test trailer
 *   REBOOTING    boot_request_upgrade done, sys_reboot pending
 *   FAILED       last OTA aborted; transient -- reset to IDLE on next begin
 */
enum ota_phase {
	OTA_IDLE = 0,
	OTA_PREPARING,
	OTA_TRANSFERRING,
	OTA_MARKING,
	OTA_REBOOTING,
	OTA_FAILED,
};

enum ota_source {
	OTA_SRC_NONE = 0,
	OTA_SRC_BLE,
	OTA_SRC_SD,
};

/* Current pipeline state. Safe to call from any context. */
enum ota_phase  ota_phase_get(void);
enum ota_source ota_source_get(void);
const char     *ota_phase_name(enum ota_phase p);
const char     *ota_source_name(enum ota_source s);

/* True iff phase != IDLE/FAILED. Used by logctl to inhibit auto-sleep,
 * by other subsystems to detect "device is busy doing OTA, don't
 * interfere". */
bool ota_in_progress(void);

/* Initializes the mcumgr DFU callback so the BLE OTA path drives the
 * same display indicator as the SD-card path AND sets the in-progress
 * flag for logctl. Idempotent. */
void ota_dfu_hooks_init(void);

/* Unified state-machine entry points. Both BLE-mcumgr and SD-card OTA
 * paths drive the same transitions so the device sees one consistent
 * view of "is OTA happening, what stage, where did it come from".
 *
 * ota_begin(): refuse if another OTA already in flight; otherwise
 *   PREPARING -> stop logger -> TRANSFERRING. Returns -EBUSY if blocked.
 * ota_mark_start():    upload finished; about to write test trailer
 * ota_reboot_start():  trailer written; about to sys_reboot
 * ota_end_failure():   any step failed; goes to FAILED then idle
 * ota_end_abort():     transport ended without success or failure
 *                      (e.g. host disconnected mid-upload); goes to IDLE
 */
int  ota_begin(enum ota_source src, const char *banner);
void ota_mark_start(void);
void ota_reboot_start(void);
void ota_end_failure(void);
void ota_end_abort(void);

#endif /* OTA_H_ */
