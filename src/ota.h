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

/* True while a BLE mcumgr-SMP image upload is in flight (between
 * DFU_STARTED and DFU_STOPPED/DFU_PENDING events). Used by logctl to
 * inhibit the auto-sleep timer so the device doesn't drop into LS_IDLE
 * mid-upload. Safe to call from any context. */
bool ota_in_progress(void);

/* Initializes the mcumgr DFU callback so the BLE OTA path drives the
 * same display indicator as the SD-card path AND sets the in-progress
 * flag for logctl. Idempotent. */
void ota_dfu_hooks_init(void);

#endif /* OTA_H_ */
