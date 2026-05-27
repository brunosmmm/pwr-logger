#ifndef CFGFILE_H_
#define CFGFILE_H_

/*
 * SD-backed config overlay: parse /SD:/pwr.cfg (INI-style key=value lines)
 * and apply each key via config_set(). Unknown keys / parse failures are
 * logged at WRN level and skipped -- a corrupt or out-of-date file never
 * blocks the rest of the application.
 *
 * Caller must have storage_mount()-ed first.
 *
 * Returns the number of keys successfully applied, or negative errno on
 * a file-level failure (file not found, read error, etc).
 */
int cfgfile_apply_from_sd(void);

/*
 * Dump the current config (every cfg_items[] entry) to /SD:/pwr.cfg as
 * key=value lines. Useful for capturing live-tuned settings to disk for
 * later re-import. Truncates any existing file.
 */
int cfgfile_export_to_sd(void);

/* Path constant (so docs / CLI help can reference it). */
#define CFGFILE_PATH  "/SD:/pwr.cfg"

#endif /* CFGFILE_H_ */
