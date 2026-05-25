#include "logger.h"
#include "app_config.h"
#include "acquisition.h"
#include "ina228.h"
#include "aggregate.h"
#include "tdms.h"
#include "storage.h"
#include "timebase.h"
#include "ble_stream.h"

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

/* Fast-channel (INA240 -> SAADC) code->amps, same scaling as tdms.c. */
#define FAST_SLOPE_A_PER_CODE ((double)ADC_VFULL_SCALE_V / \
	((double)(1u << ADC_BITS) * (double)INA240_GAIN * (double)INA240_SHUNT_OHMS))
#define FAST_INTERCEPT_A (-(double)INA240_VREF_V / \
	((double)INA240_GAIN * (double)INA240_SHUNT_OHMS))

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
	float fast_avg = 0.0f;   /* latest agg mean code, for fast-channel readout */
	unsigned wins = 0;       /* agg windows taken / sec (BLE producer rate) */
	unsigned nerr = 0;       /* bt_gatt_notify failures / sec (TX buffer drops) */

	while (s_running) {
		struct acq_block blk;

		if (k_msgq_get(&acq_msgq, &blk, K_MSEC(100)) == 0) {
			uint64_t t = timebase_extend(blk.t_capture);

			agg_push(blk.samples, blk.count);           /* always: live view */
			if (s_session) {
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
		}

		int64_t now = k_uptime_get();

		if (s_session && slow_n > 0 &&
		    (now - t_flush) >= SLOW_FLUSH_INTERVAL_MS) {
			tdms_write_slow(&s_tdms, slow, slow_n);
			storage_sync(&s_file);
			slow_n = 0;
			t_flush = now;
		}

		if (now - t_report >= 1000) {
			int fast_ua = (int)((FAST_SLOPE_A_PER_CODE * fast_avg +
					     FAST_INTERCEPT_A) * 1e6);

			LOG_INF("acq: %llu samples/s, %u drops [%s]  fast_I=%d uA (code %d)  ble: wins=%u nerr=%u",
				samples, acq_drops(), s_session ? "logging" : "monitor",
				fast_ua, (int)fast_avg, wins, nerr);
			samples = 0;
			wins = 0;
			nerr = 0;
			t_report = now;
		}

		struct agg_window w;

		if (agg_take(&w)) {
			fast_avg = w.avg;
			wins++;
			if (ble_stream_publish(&w) < 0) {
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
	return 0;
}

int logger_start(void)
{
	/* SD session is OPTIONAL: with no card we still acquire (monitor mode). */
	s_session = false;
	if (storage_mount() == 0 &&
	    storage_session_begin(&s_file) == 0 &&
	    tdms_open(&s_tdms, &s_file) == 0) {
		s_session = true;
		LOG_INF("recording to SD");
	} else {
		LOG_WRN("no SD session -> MONITOR mode (live view only)");
	}

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
