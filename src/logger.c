#include "logger.h"
#include "app_config.h"
#include "acquisition.h"
#include "ina228.h"
#include "aggregate.h"
#include "tdms.h"
#include "storage.h"
#include "timebase.h"
#include "ble_stream.h"
#include "cal.h"
#include "config.h"
#include "stats.h"

#include <zephyr/fs/fs.h>

#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(logger, LOG_LEVEL_INF);

#define WRITER_STACK_SIZE 4096
#define WRITER_PRIO       5
#define SLOW_BATCH_MAX    16

K_THREAD_STACK_DEFINE(writer_stack, WRITER_STACK_SIZE);
static struct k_thread writer_thread;
static k_tid_t writer_tid;

static struct storage_file s_file;
static struct tdms_writer  s_tdms;
static volatile bool s_running;
static volatile bool s_rotate; /* writer cuts a fresh file at the next boundary */
static bool s_session; /* true = recording to SD; false = monitor-only */

/* SD-usage cache + samples-per-second snapshot. Written only by the writer
 * thread (which also holds the FatFs lock for its own fs_write calls), so no
 * concurrency hazard with the rest of the file system. Read by the UI thread
 * via logger_sd_usage() / logger_samples_per_sec() under a small mutex. The
 * 64-bit fields can't be atomic on Cortex-M4 in plain C; the rate is a u32
 * and uses a relaxed atomic_t. */
static struct k_mutex s_sd_lock;
static uint64_t       s_sd_file_b;
static uint64_t       s_sd_free_b;
static uint64_t       s_sd_total_b;
static atomic_t       s_samples_per_sec;

/* Marker pipeline: any thread can call logger_marker(msg); the writer thread
 * drains marker_q each loop iteration and calls tdms_write_event() for each
 * pending message. Up to 8 in-flight markers; further calls return -EBUSY
 * silently rather than blocking the caller. */
#define MARKER_MAX_LEN  32
struct marker_item { char msg[MARKER_MAX_LEN]; };
K_MSGQ_DEFINE(marker_q, sizeof(struct marker_item), 8, 4);

int logger_marker(const char *msg)
{
	struct marker_item m = { 0 };
	if (msg && *msg) {
		strncpy(m.msg, msg, sizeof(m.msg) - 1);
	}
	return k_msgq_put(&marker_q, &m, K_NO_WAIT);
}

/* Fast-channel (INA240 -> SAADC) code->amps, same scaling as tdms.c. */
#define FAST_SLOPE_A_PER_CODE ((double)ADC_VFULL_SCALE_V / \
	((double)(1u << ADC_BITS) * (double)INA240_GAIN * (double)INA240_SHUNT_OHMS))
#define FAST_INTERCEPT_A (-(double)INA240_VREF_V / \
	((double)INA240_GAIN * (double)INA240_SHUNT_OHMS))

/* INA228 current LSB (A/LSB), matches ina228.c (IMAX / 2^19). */
#define INA228_CURRENT_LSB_A ((double)INA228_IMAX_A / 524288.0)
/* Fast window code spread above which we treat it as mid-transient and skip the
 * cal update (the long tau already rejects transients; this is belt-and-braces). */
#define CAL_STEADY_SPREAD 512

/*
 * The contract: ALWAYS drain acq_msgq (feed the live view + return buffers to
 * the pool); persisting to SD is conditional on a session being open. This keeps
 * acquisition real-time-safe whether or not an SD card is present.
 */
static void writer_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	struct ina228_sample slow[SLOW_BATCH_MAX];
	int slow_n = 0;
	uint64_t samples = 0;
	int64_t t_report = k_uptime_get();
	int64_t t_flush = t_report;
	float fast_avg = 0.0f;   /* latest agg mean code (corrected), for the readout */
	unsigned wins = 0;       /* agg windows taken / sec (BLE producer rate) */
	unsigned nerr = 0;       /* bt_gatt_notify failures / sec (TX buffer drops) */
	float fast_i_raw = 0.0f; /* latest RAW fast current (A), for the cal loop */
	bool fast_steady = false;
	int64_t t_cal = t_report;

	while (s_running) {
		struct acq_block blk;

		if (k_msgq_get(&acq_msgq, &blk, K_MSEC(100)) == 0) {
			uint64_t t = timebase_extend(blk.t_capture);

			agg_push(blk.samples, blk.count, t);        /* RAW -> live view + cal; t = first-sample tick */
			if (s_session) {
				/* Bake the INA228-referenced offset into the stored codes so
				 * the SD data agrees with the precision channel (no metadata). */
				int off = (int)lroundf(cal_offset_a() / (float)FAST_SLOPE_A_PER_CODE);

				if (off != 0) {
					for (uint16_t i = 0; i < blk.count; i++) {
						blk.samples[i] = (int16_t)(blk.samples[i] - off);
					}
				}
				tdms_write_fast(&s_tdms, blk.samples, blk.count, t);
			}
			samples += blk.count;
			acq_block_release(&blk);                    /* always */
		}

		struct ina228_sample s;

		while (slow_n < SLOW_BATCH_MAX &&
		       k_msgq_get(&ina228_msgq, &s, K_NO_WAIT) == 0) {
			slow[slow_n++] = s;
			ble_stream_publish_slow(&s);   /* live INA228 stream (throttled) */

			/* Cross-calibrate the fast zero against the INA228 (same series
			 * current). Slow EMA (cal_tau_s) tracks thermal drift only. */
			int64_t tnow = k_uptime_get();
			float i_ina = (float)(s.current_raw * INA228_CURRENT_LSB_A);

			cal_update(fast_i_raw, i_ina, (float)(tnow - t_cal) / 1000.0f,
				   fast_steady);
			t_cal = tnow;
		}

		int64_t now = k_uptime_get();

		if (s_session && slow_n > 0 &&
		    (now - t_flush) >= SLOW_FLUSH_INTERVAL_MS) {
			tdms_write_slow(&s_tdms, slow, slow_n);
			storage_sync(&s_file);
			slow_n = 0;
			t_flush = now;
		}

		/* Drain any pending marker requests. Cheap: a single small
		 * meta-only segment per marker (~110 B). */
		if (s_session) {
			struct marker_item m;
			while (k_msgq_get(&marker_q, &m, K_NO_WAIT) == 0) {
				(void)tdms_write_event(&s_tdms,
					timebase_extend(timebase_now32()),
					m.msg);
			}
		}

		if (now - t_report >= 1000) {
			int fast_ua = (int)((FAST_SLOPE_A_PER_CODE * fast_avg +
					     FAST_INTERCEPT_A) * 1e6);

			LOG_INF("acq: %llu samples/s, %u drops [%s]  fast_I=%d uA (code %d)  ble: wins=%u nerr=%u",
				samples, acq_drops(), s_session ? "logging" : "monitor",
				fast_ua, (int)fast_avg, wins, nerr);

			/* Publish the per-second snapshots while we're already at a
			 * 1 s boundary (and holding the FatFs lock via fs_* calls in
			 * the same thread -- statvfs/stat are safe here). */
			atomic_set(&s_samples_per_sec, (atomic_val_t)samples);
			if (s_session) {
				struct fs_statvfs st;
				struct fs_dirent ent;
				uint64_t file_b = 0, free_b = 0, total_b = 0;
				if (fs_statvfs("/SD:", &st) == 0) {
					free_b  = (uint64_t)st.f_bfree  * (uint64_t)st.f_frsize;
					total_b = (uint64_t)st.f_blocks * (uint64_t)st.f_frsize;
				}
				const char *path = storage_session_path();
				if (path && *path && fs_stat(path, &ent) == 0) {
					file_b = (uint64_t)ent.size;
				}
				k_mutex_lock(&s_sd_lock, K_FOREVER);
				s_sd_file_b = file_b;
				s_sd_free_b = free_b;
				s_sd_total_b = total_b;
				k_mutex_unlock(&s_sd_lock);
			}

			samples = 0;
			wins = 0;
			nerr = 0;
			t_report = now;
		}

		struct agg_window w;

		if (agg_take(&w)) {
			/* RAW fast current feeds the cal loop; the offset corrects the
			 * window we publish so the live fast channel matches the INA228. */
			fast_i_raw = (float)(FAST_SLOPE_A_PER_CODE * w.avg + FAST_INTERCEPT_A);
			fast_steady = (w.max - w.min) < CAL_STEADY_SPREAD;

			int off = (int)lroundf(cal_offset_a() / (float)FAST_SLOPE_A_PER_CODE);
			struct agg_window wc = w;

			wc.avg -= off;
			wc.min -= off;
			wc.max -= off;
			fast_avg = wc.avg;
			wins++;
			if (ble_stream_publish(&wc) < 0) {
				nerr++;
			}
		}

		/* Seamless rotate: close + reopen at a buffer boundary. Acquisition
		 * keeps filling acq_msgq (the ~660 ms buffer slack covers it). */
		if (s_rotate && s_session) {
			storage_sync(&s_file);
			tdms_close(&s_tdms);
			storage_session_end(&s_file);
			if (storage_session_begin(&s_file) == 0 &&
			    tdms_open(&s_tdms, &s_file) == 0) {
				LOG_INF("rotated to new session file");
			} else {
				LOG_ERR("rotate failed -> session ended");
				s_session = false;
			}
			s_rotate = false;
		}
	}
}

int logger_init(void)
{
	agg_reset();
	k_mutex_init(&s_sd_lock);
	return 0;
}

void logger_sd_usage(uint64_t *file_b, uint64_t *free_b, uint64_t *total_b)
{
	k_mutex_lock(&s_sd_lock, K_FOREVER);
	if (file_b)  *file_b  = s_sd_file_b;
	if (free_b)  *free_b  = s_sd_free_b;
	if (total_b) *total_b = s_sd_total_b;
	k_mutex_unlock(&s_sd_lock);
}

uint32_t logger_samples_per_sec(void)
{
	return (uint32_t)atomic_get(&s_samples_per_sec);
}

int logger_start(void)
{
	/* SD session is OPTIONAL: with no card we still acquire (monitor mode). */
	s_session = false;
	(void)storage_unmount();   /* drop any stale mount (idle rail-cycle / STOPPED) */
	if (storage_mount() == 0 &&
	    storage_session_begin(&s_file) == 0 &&
	    tdms_open(&s_tdms, &s_file) == 0) {
		s_session = true;
		LOG_INF("recording to SD");
	} else {
		LOG_WRN("no SD session -> MONITOR mode (live view only)");
	}

	/* Session-start side effects on the stats engine, all opt-in via config. */
	const struct app_cfg *cfg = config_get();
	if (cfg->stats_reset_on_start)  { stats_session_reset(); }
	if (cfg->hold_reset_on_start)   { stats_hold_clear(STATS_CH_ALL); }
	if (cfg->energy_reset_on_start) { stats_energy_clear(); }

	/* Per-session drop counter: zero at every logger_start so the FILE/SYS
	 * pages show drops scoped to the current recording, not project lifetime. */
	acq_drops_reset();

	s_running = true;
	writer_tid = k_thread_create(&writer_thread, writer_stack,
				     K_THREAD_STACK_SIZEOF(writer_stack),
				     writer_fn, NULL, NULL, NULL,
				     WRITER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(writer_tid, "logger");

	acq_start();
	ina228_start();
	LOG_INF("acquisition started");
	return 0;
}

int logger_rotate(void)
{
	if (s_session) {
		s_rotate = true;
	}
	return 0;
}

int logger_stop(void)
{
	acq_stop();
	ina228_stop();

	s_running = false;
	if (writer_tid != NULL) {
		k_thread_join(&writer_thread, K_FOREVER);
		writer_tid = NULL;
	}
	if (s_session) {
		tdms_close(&s_tdms);
		storage_session_end(&s_file);
		s_session = false;
	}
	LOG_INF("acquisition stopped");
	return 0;
}
