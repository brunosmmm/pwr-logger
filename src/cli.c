#include "config.h"
#include "clock.h"
#include "logctl.h"
#include "power.h"
#include "cal.h"
#include "stats.h"
#include "ui.h"
#include "cfgfile.h"
#include "logger.h"
#include "controls.h"
#include "ota.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/crc.h>
#include <zephyr/dfu/mcuboot.h>
#include "storage.h"
#include "usbd_pwr_bulk.h"

/*
 * Console front-end (Zephyr shell over the CDC-ACM USB console). Commands are
 * thin wrappers over config/clock (and later logctl). Root command: `pwr`.
 */

static void fmt_utc(time_t t, char *buf, size_t len)
{
	struct tm tm;

	gmtime_r(&t, &tm);
	snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	char b[32];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (clk_has_time()) {
		fmt_utc(clk_now(), b, sizeof(b));
	} else {
		strcpy(b, "(unset)");
	}
	shell_print(sh, "state:  %s", logctl_state_name(logctl_state()));
	shell_print(sh, "time:   %s (synced=%d)", b, clk_is_synced());
	shell_print(sh, "uptime: %lld ms", k_uptime_get());
	shell_print(sh, "fast cal offset: %d uA", (int)(cal_offset_a() * 1e6f));
	return 0;
}

static int cmd_log(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_LOG_START);
	shell_print(sh, "logging");
	return 0;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_LOG_STOP);
	shell_print(sh, "stopped");
	return 0;
}

static int cmd_rotate(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_ROTATE);
	shell_print(sh, "rotate requested");
	return 0;
}

static int cmd_offload(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_OFFLOAD_ON);
	shell_print(sh, "offload: stopping + releasing SD; mount the USB drive, then 'pwr resume'");
	return 0;
}

static int cmd_resume(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_OFFLOAD_OFF);
	shell_print(sh, "resuming logging (unmount the USB drive on the host first)");
	return 0;
}

static int cmd_poweroff(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_POWEROFF);
	shell_print(sh, "powering off");
	return 0;
}

static int cmd_lowbatt(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	logctl_post(EV_LOW_BATT);
	shell_print(sh, "simulated low-battery event");
	return 0;
}

static int cmd_usb(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: pwr usb <on|off>");
		return -EINVAL;
	}
	if (!strcmp(argv[1], "off")) {
		logctl_post(EV_USB_OFF);
		shell_print(sh, "simulated VBUS removed");
	} else {
		logctl_post(EV_USB_ON);
		shell_print(sh, "simulated VBUS ready");
	}
	return 0;
}

static int cmd_batt(const struct shell *sh, size_t argc, char **argv)
{
	int mv;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	if (logctl_state() != LS_LOGGING) {
		power_batt_sample();   /* SAADC free -> fresh reading */
	}
	mv = power_batt_mv();
	if (mv < 0) {
		shell_print(sh, "battery: unknown (not sampled yet)");
	} else {
		shell_print(sh, "battery (VDDH): %d.%03d V%s", mv / 1000, mv % 1000,
			    logctl_state() == LS_LOGGING ? "  (cached; SAADC busy logging)" : "");
	}
	return 0;
}

static int cmd_time_get(const struct shell *sh, size_t argc, char **argv)
{
	char b[32];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	fmt_utc(clk_now(), b, sizeof(b));
	shell_print(sh, "%lld  %s  synced=%d", (long long)clk_now(), b, clk_is_synced());
	return 0;
}

static int cmd_time_set(const struct shell *sh, size_t argc, char **argv)
{
	time_t t;
	char b[32];

	if (argc < 2) {
		shell_error(sh, "usage: pwr time set <unix-epoch-seconds>");
		return -EINVAL;
	}
	t = (time_t)strtoll(argv[1], NULL, 0);
	clk_set(t);
	fmt_utc(t, b, sizeof(b));
	shell_print(sh, "set %s", b);
	return 0;
}

static int cmd_cfg_get(const struct shell *sh, size_t argc, char **argv)
{
	char v[16];

	if (argc < 2) {
		shell_error(sh, "usage: pwr config get <key>");
		return -EINVAL;
	}
	if (config_get_str(argv[1], v, sizeof(v))) {
		shell_error(sh, "no such key '%s'", argv[1]);
		return -ENOENT;
	}
	shell_print(sh, "%s = %s", argv[1], v);
	return 0;
}

static int cmd_cfg_set(const struct shell *sh, size_t argc, char **argv)
{
	int rc;

	if (argc < 3) {
		shell_error(sh, "usage: pwr config set <key> <value>");
		return -EINVAL;
	}
	rc = config_set(argv[1], argv[2]);
	if (rc) {
		shell_error(sh, "set failed (%d)", rc);
		return rc;
	}
	shell_print(sh, "ok");
	return 0;
}

static void list_cb(const char *key, const char *val, void *ctx)
{
	shell_print((const struct shell *)ctx, "  %-16s = %s", key, val);
}

static int cmd_cfg_list(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	config_foreach(list_cb, (void *)sh);
	return 0;
}

static int cmd_cfg_import(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	int n = cfgfile_apply_from_sd();
	if (n < 0) {
		shell_error(sh, "import failed: %d", n);
		return n;
	}
	shell_print(sh, "imported %d key(s) from %s", n, CFGFILE_PATH);
	return 0;
}

static int cmd_cfg_export(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	int rc = cfgfile_export_to_sd();
	if (rc) {
		shell_error(sh, "export failed: %d", rc);
		return rc;
	}
	shell_print(sh, "exported config to %s", CFGFILE_PATH);
	return 0;
}

static const char *const ch_names[STATS_CH__COUNT] = {
	"fast_i", "slow_i", "slow_v", "slow_p", "slow_t",
};
static const char *const ch_units[STATS_CH__COUNT] = {
	"A", "A", "V", "W", "C",
};

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	uint16_t win = config_get()->stats_window_s;
	shell_print(sh, "rolling window: %u s", (unsigned)win);
	shell_print(sh, "  channel    mean        min         max         n");
	for (int i = 0; i < STATS_CH__COUNT; i++) {
		struct stats_window w;
		if (!stats_window_get((enum stats_ch)i, win, &w)) {
			shell_print(sh, "  %-8s   (no data)", ch_names[i]);
			continue;
		}
		shell_print(sh, "  %-8s  % .4g %s   % .4g %s   % .4g %s   %u",
			    ch_names[i], (double)w.mean, ch_units[i],
			    (double)w.min, ch_units[i],
			    (double)w.max, ch_units[i], (unsigned)w.n);
	}
	return 0;
}

static int cmd_hold(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (int i = 0; i < STATS_CH__COUNT; i++) {
		struct stats_hold h;
		if (!stats_hold_get((enum stats_ch)i, &h)) {
			shell_print(sh, "  %-8s   (no data)", ch_names[i]);
			continue;
		}
		int64_t age = k_uptime_get() - h.since_uptime_ms;
		shell_print(sh, "  %-8s  min % .4g %s  max % .4g %s  for %lld ms",
			    ch_names[i], (double)h.min, ch_units[i],
			    (double)h.max, ch_units[i], (long long)age);
	}
	return 0;
}

static int cmd_hold_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	stats_hold_clear(STATS_CH_ALL);
	shell_print(sh, "hold cleared (all channels)");
	return 0;
}

static int cmd_energy(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	double wh = stats_energy_wh();
	double q  = stats_energy_q_as();
	int64_t ms = stats_energy_elapsed_ms();
	shell_print(sh, "  E  = %.4g mWh   ( %.6g Wh )",
		    wh * 1000.0, wh);
	shell_print(sh, "  Q  = %.4g mAh   ( %.6g As )",
		    q * 1000.0 / 3600.0, q);
	shell_print(sh, "  t  = %lld ms",  (long long)ms);
	return 0;
}

static int cmd_energy_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	stats_energy_clear();
	shell_print(sh, "energy accumulator reset");
	return 0;
}

static int cmd_marker(const struct shell *sh, size_t argc, char **argv)
{
	const char *msg = (argc >= 2) ? argv[1] : "shell";
	int rc = logger_marker(msg);
	if (rc) {
		shell_error(sh, "marker queue full or no session (%d)", rc);
		return rc;
	}
	shell_print(sh, "marker queued: %s", msg);
	return 0;
}

/* `pwr ui page N` -- jump to a specific UI page (0..PAGE__COUNT-1).
 * Mostly diagnostic; the operator usually uses ROTATE button (Phase 3). */
static const char *const page_names[PAGE__COUNT] = {
	"LIVE", "ROLLING", "PEAKS", "HOLD", "ENERGY", "FILE", "SYSTEM",
};

static int cmd_ui_page(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "current page: %d (%s)",
			    (int)ui_get_page(), page_names[ui_get_page()]);
		shell_print(sh, "available:");
		for (int i = 0; i < PAGE__COUNT; i++) {
			shell_print(sh, "  %d  %s", i, page_names[i]);
		}
		return 0;
	}
	int p = atoi(argv[1]);
	if (p < 0 || p >= PAGE__COUNT) {
		shell_error(sh, "page out of range (0..%d)", PAGE__COUNT - 1);
		return -EINVAL;
	}
	ui_set_page((enum ui_page)p);
	shell_print(sh, "page = %d (%s)", p, page_names[p]);
	return 0;
}

static int cmd_ui_next(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	ui_next_page();
	shell_print(sh, "page = %d (%s)", (int)ui_get_page(), page_names[ui_get_page()]);
	return 0;
}

static int cmd_ui_icons(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t ms = 5000;

	if (argc >= 2) {
		ms = (uint32_t)strtoul(argv[1], NULL, 0);
	}
	ui_icon_test(ms);
	shell_print(sh, "icon test: %u ms (0x40..0x47 across status bar)", ms);
	return 0;
}

/* ---- automated-test offload: 'pwr ls' + 'pwr download <name>' ------------
 *
 * Streams a TDMS session file back over the CDC console as a self-framed
 * binary blob, so a host CI script never has to do the MSC mount / copy /
 * unmount dance to retrieve data.
 *
 *   header = 'P' 'W' 'R' 0x01   (4 magic bytes)
 *            <size:u32 LE>
 *   body   = <size> raw bytes
 *   footer = <crc32_ieee:u32 LE>
 *
 * Host: scan stdin for the magic, read 4-byte size, read body, read CRC,
 * verify. See tools/pwrdl.py. CRC is the standard zlib CRC-32 (poly
 * 0xEDB88320, init/xorout 0xFFFFFFFF) so any language with a stdlib CRC
 * implementation can verify trivially.
 *
 * The transfer is raw uart_poll_out so it bypasses the shell's text-mode
 * formatting; the shell prompt that re-appears after the command finishes
 * is just trailing noise the host can ignore. */

/* FatFs/SD over SPI pays per-call FAT-lookup + sector-iteration overhead
 * that's mostly fixed; larger reads amortize it. With ping-pong prefetch
 * (see cmd_download) reads run concurrent to USB shipping, so DL_CHUNK
 * mainly controls the SD-side amortization curve.
 *
 * 16 KB measured: 16 KB -> 560-563 KB/s, 32 KB -> 578 KB/s (+3%). 32 KB
 * pushes RAM to 98.3% leaving no headroom for future features or
 * worst-case thread stack growth; 16 KB lands at ~86% with the same
 * download buffers ping-ponging. The 3% throughput we leave on the
 * table is the cost of keeping ~12% RAM in reserve. */
#define DL_CHUNK 16384

static const struct device *console_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* Batched write to the CDC console.
 *
 * The real bottleneck on the previous uart_poll_out path was that the CDC
 * ACM driver schedules its TX worker with K_MSEC(1) after every poll_out
 * call (subsys/usb/device_next/class/usbd_cdc_acm.c:1018). Bytes accumulate
 * in the ring buffer for up to 1 ms before the worker drains them to USB --
 * capping us at ~160 KB/s CPU-bound regardless of buffer pool size.
 *
 * Two changes here:
 * 1. uart_fifo_fill puts the whole chunk in the ring buffer in one critical
 *    section instead of byte-at-a-time poll_out calls. CONFIG_USBD_CDC_ACM_
 *    WORKQUEUE=n makes check_wq_ctx() always true so it's safe from any
 *    thread.
 * 2. uart_irq_tx_enable submits the worker immediately (no K_MSEC(1) delay,
 *    cdc_acm.c:734), so the ring buffer drains without waiting for a timer
 *    tick.
 *
 * We never install our own UART IRQ callback, so the shell's RX handler
 * stays intact. The shell does observe TX-ready events (since we enable
 * TX irq), but it has nothing queued during cmd_download so the events
 * are no-ops on its side. */

/* Stream a buffer over the dedicated vendor-class bulk-IN endpoint
 * (Interface 3, EP 0x84). pwr_bulk_send blocks only when all
 * PWR_BULK_BUF_COUNT (=8) in-flight transfer slots are busy, giving us
 * multiple concurrent USB transfers and saturating the FS bulk endpoint.
 * Falls back to a CDC-shell error if the host hasn't claimed the interface,
 * but in practice the host script does that BEFORE issuing 'pwr download'. */
static void dl_write(const uint8_t *data, size_t len)
{
	(void)pwr_bulk_send(data, len);
}

static void dl_write_u32_le(uint32_t v)
{
	uint8_t b[4] = {
		(uint8_t)v, (uint8_t)(v >> 8),
		(uint8_t)(v >> 16), (uint8_t)(v >> 24),
	};

	dl_write(b, 4);
}

static int cmd_ls(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);

	bool we_mounted = (storage_mount() == 0);

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	int rc = fs_opendir(&dir, "/SD:");

	if (rc != 0) {
		shell_error(sh, "opendir(/SD:) failed: %d", rc);
		if (we_mounted) {
			(void)storage_unmount();
		}
		return -1;
	}

	struct fs_dirent ent;
	int count = 0;
	uint64_t total = 0;

	while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
		if (ent.type == FS_DIR_ENTRY_FILE) {
			shell_print(sh, "%-32s %u", ent.name, (unsigned)ent.size);
			count++;
			total += ent.size;
		}
	}
	(void)fs_closedir(&dir);
	shell_print(sh, "(%d files, %llu bytes)", count, (unsigned long long)total);

	if (we_mounted) {
		(void)storage_unmount();
	}
	return 0;
}

/* Two ping-pong SD buffers + system-workqueue read worker so the SD-read
 * path (which serializes inside FatFs + the SDHC SPI driver) overlaps
 * with USB shipping. While the shell thread is in pwr_bulk_send draining
 * buffer A to the bulk-IN endpoint, the workqueue runs fs_read into
 * buffer B; on the next iteration they swap. fp is touched by exactly
 * one thread at a time so no extra synchronization on the file handle.
 *
 * BSS: 2 x DL_CHUNK = 16 KB.
 */
static uint8_t dl_buf[2][DL_CHUNK];
static char    dl_path[80];

struct dl_read_req {
	struct k_work     work;
	struct fs_file_t *fp;
	uint8_t          *buf;
	size_t            to_read;
	ssize_t           result;
	struct k_sem      done;
};

static struct dl_read_req dl_req;

static void dl_read_worker(struct k_work *w)
{
	struct dl_read_req *r = CONTAINER_OF(w, struct dl_read_req, work);

	r->result = fs_read(r->fp, r->buf, r->to_read);
	k_sem_give(&r->done);
}

static int cmd_download(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: pwr download <filename>");
		shell_print(sh,  "       (use 'pwr ls' for the list)");
		return -1;
	}

	bool we_mounted = (storage_mount() == 0);

	snprintf(dl_path, sizeof(dl_path), "/SD:/%s", argv[1]);

	struct fs_dirent ent;
	int rc = fs_stat(dl_path, &ent);

	if (rc != 0 || ent.type != FS_DIR_ENTRY_FILE) {
		shell_error(sh, "no such file: %s (rc=%d)", dl_path, rc);
		if (we_mounted) {
			(void)storage_unmount();
		}
		return -1;
	}

	struct fs_file_t fp;

	fs_file_t_init(&fp);
	rc = fs_open(&fp, dl_path, FS_O_READ);
	if (rc != 0) {
		shell_error(sh, "open failed: %d", rc);
		if (we_mounted) {
			(void)storage_unmount();
		}
		return -1;
	}

	/* Header: magic + LE size. */
	const uint8_t magic[4] = { 'P', 'W', 'R', 0x01 };

	dl_write(magic, 4);
	dl_write_u32_le((uint32_t)ent.size);

	k_work_init(&dl_req.work, dl_read_worker);
	k_sem_init(&dl_req.done, 0, 1);
	dl_req.fp = &fp;

	uint32_t crc = 0;
	size_t remaining = ent.size;
	int cur = 0;

	/* Prime buffer 0 synchronously so we have data to ship on the
	 * first iteration. From here on the worker fills the other buffer
	 * concurrently with pwr_bulk_send draining this one. */
	ssize_t n = 0;

	if (remaining > 0) {
		n = fs_read(&fp, dl_buf[cur],
			    MIN((size_t)DL_CHUNK, remaining));
		if (n > 0) {
			remaining -= (size_t)n;
		}
	}

	while (n > 0) {
		bool prefetch = (remaining > 0);

		if (prefetch) {
			dl_req.buf     = dl_buf[1 - cur];
			dl_req.to_read = MIN((size_t)DL_CHUNK, remaining);
			dl_req.result  = 0;
			k_sem_reset(&dl_req.done);
			k_work_submit(&dl_req.work);
		}

		dl_write(dl_buf[cur], (size_t)n);
		crc = crc32_ieee_update(crc, dl_buf[cur], (size_t)n);

		if (!prefetch) {
			break;
		}

		(void)k_sem_take(&dl_req.done, K_FOREVER);
		n = dl_req.result;
		if (n > 0) {
			remaining -= (size_t)n;
		}
		cur ^= 1;
	}
	(void)fs_close(&fp);

	/* Footer: CRC32 LE. */
	dl_write_u32_le(crc);

	/* Wait for all in-flight bulk transfers to complete before we return. */
	pwr_bulk_flush();

	if (we_mounted) {
		(void)storage_unmount();
	}
	return 0;
}

static int cmd_led_diag(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(sh, "usage: pwr led <0|1|z|auto>");
		shell_print(sh,  "  0    drive LOW  (output, no pull)");
		shell_print(sh,  "  1    drive HIGH (output, no pull)");
		shell_print(sh,  "  z    tristate  (input no pull -- glow = external)");
		shell_print(sh,  "  auto resume the state-driven blink engine");
		return -1;
	}
	const char *a = argv[1];
	int mode;

	if (!strcmp(a, "0"))      mode = 0;
	else if (!strcmp(a, "1")) mode = 1;
	else if (!strcmp(a, "z")) mode = -1;
	else                      mode = 99;   /* anything else -> resume */

	controls_led_diag(mode);
	shell_print(sh, "led diag: %s",
		    mode == 0 ? "LOW"  : mode == 1 ? "HIGH" :
		    mode == -1 ? "TRISTATE (input no-pull)" : "resumed");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(time_sub,
	SHELL_CMD(get, NULL, "show current UTC time", cmd_time_get),
	SHELL_CMD(set, NULL, "set time: pwr time set <epoch>", cmd_time_set),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(cfg_sub,
	SHELL_CMD(get,    NULL, "get <key>", cmd_cfg_get),
	SHELL_CMD(set,    NULL, "set <key> <value>", cmd_cfg_set),
	SHELL_CMD(list,   NULL, "list all config", cmd_cfg_list),
	SHELL_CMD(import, NULL, "load /SD:/pwr.cfg into NVS", cmd_cfg_import),
	SHELL_CMD(export, NULL, "write current config to /SD:/pwr.cfg", cmd_cfg_export),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(hold_sub,
	SHELL_CMD(show,  NULL, "print HOLD latches",   cmd_hold),
	SHELL_CMD(clear, NULL, "clear all HOLD",       cmd_hold_clear),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(energy_sub,
	SHELL_CMD(show,  NULL, "print accumulators",   cmd_energy),
	SHELL_CMD(clear, NULL, "reset accumulators",   cmd_energy_clear),
	SHELL_SUBCMD_SET_END);

static const char *ota_swap_str(int t)
{
	switch (t) {
	case BOOT_SWAP_TYPE_NONE:    return "none";
	case BOOT_SWAP_TYPE_TEST:    return "test (will swap, revert if not confirmed)";
	case BOOT_SWAP_TYPE_PERM:    return "permanent (will swap, stick)";
	case BOOT_SWAP_TYPE_REVERT:  return "revert (rolling back unconfirmed image)";
	case BOOT_SWAP_TYPE_FAIL:    return "fail";
	default:                     return "unknown";
	}
}

static int cmd_ota_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "current image:  %s",
		    ota_test_pending() ? "UNCONFIRMED (will revert on next reset)" :
					 "confirmed");
	shell_print(sh, "pending swap:   %s", ota_swap_str(ota_pending_swap_type()));
	shell_print(sh, "update file:    %s", OTA_FILE_PATH);
	return 0;
}

static void ota_progress_print(const char *stage, int rc, void *user)
{
	const struct shell *sh = user;

	if (rc == 0) {
		shell_print(sh, "ota: %s ok", stage);
	} else {
		shell_print(sh, "ota: %s err=%d", stage, rc);
	}
}

static int cmd_ota_apply(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "ota: streaming %s to slot1; device will reboot on success",
		    OTA_FILE_PATH);
	int rc = ota_apply_from_sd(ota_progress_print, (void *)sh);

	/* On success ota_apply_from_sd reboots; we only reach here on failure. */
	shell_error(sh, "ota: failed (%d)", rc);
	return rc;
}

static int cmd_ota_confirm(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	ota_confirm_current();
	shell_print(sh, "ota: current image confirmed");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(ota_sub,
	SHELL_CMD(status,  NULL, "show OTA / pending-swap state", cmd_ota_status),
	SHELL_CMD(apply,   NULL, "stream /SD:/firmware.bin to slot1, mark TEST, reboot",
		  cmd_ota_apply),
	SHELL_CMD(confirm, NULL, "confirm current image (sticky)", cmd_ota_confirm),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(ui_sub,
	SHELL_CMD(page,  NULL, "page [N]: show or set page", cmd_ui_page),
	SHELL_CMD(next,  NULL, "advance to next page",       cmd_ui_next),
	SHELL_CMD(icons, NULL, "icons [ms]: draw all 8 icons across the bar for N ms (default 5000)",
		  cmd_ui_icons),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(pwr_sub,
	SHELL_CMD(status,   NULL, "device status", cmd_status),
	SHELL_CMD(log,      NULL, "start logging", cmd_log),
	SHELL_CMD(stop,     NULL, "stop logging", cmd_stop),
	SHELL_CMD(rotate,   NULL, "cut a new session file", cmd_rotate),
	SHELL_CMD(offload,  NULL, "stop + expose SD as USB drive", cmd_offload),
	SHELL_CMD(resume,   NULL, "leave offload, resume logging", cmd_resume),
	SHELL_CMD(poweroff, NULL, "clean stop + power off", cmd_poweroff),
	SHELL_CMD(batt,     NULL, "battery (VDDH) voltage", cmd_batt),
	SHELL_CMD(lowbatt,  NULL, "simulate low-battery event (test)", cmd_lowbatt),
	SHELL_CMD(usb,      NULL, "simulate VBUS on/off (test)", cmd_usb),
	SHELL_CMD(time,     &time_sub,   "time get/set", NULL),
	SHELL_CMD(config,   &cfg_sub,    "config get/set/list", NULL),
	SHELL_CMD(stats,    NULL,        "rolling min/mean/max per channel", cmd_stats),
	SHELL_CMD(hold,     &hold_sub,   "HOLD show/clear", NULL),
	SHELL_CMD(energy,   &energy_sub, "energy accumulators", NULL),
	SHELL_CMD(ui,       &ui_sub,     "UI page select", NULL),
	SHELL_CMD(marker,   NULL,        "post a TDMS marker: pwr marker [msg]", cmd_marker),
	SHELL_CMD(led,      NULL,        "led <0|1|z|auto>: diag-drive the status LED", cmd_led_diag),
	SHELL_CMD(ls,       NULL,        "list session files on SD",                cmd_ls),
	SHELL_CMD(download, NULL,        "stream <file> back as binary (PWR\\x01 + size + bytes + crc32-LE)",
		  cmd_download),
	SHELL_CMD(ota,      &ota_sub,    "OTA: SD-card update + confirm",  NULL),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pwr, &pwr_sub, "pwr-logger control", NULL);
