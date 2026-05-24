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

/*
 * Single writer thread: drains the fast and slow queues, encodes TDMS, writes to
 * SD, feeds the aggregator, and publishes the BLE live view. Acquisition never
 * touches the filesystem (REQUIREMENTS.md / IMPLEMENTATION.md task architecture).
 */
static void writer_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	struct ina228_sample slow[SLOW_BATCH_MAX];
	int slow_n = 0;
	int64_t last_flush = k_uptime_get();

	while (s_running) {
		struct acq_block blk;

		if (k_msgq_get(&acq_msgq, &blk, K_MSEC(100)) == 0) {
			uint64_t t = timebase_extend(blk.t_capture);

			tdms_write_fast(&s_tdms, blk.samples, blk.count, t);
			agg_push(blk.samples, blk.count);
			acq_block_release(&blk);
		}

		struct ina228_sample s;

		while (slow_n < SLOW_BATCH_MAX &&
		       k_msgq_get(&ina228_msgq, &s, K_NO_WAIT) == 0) {
			slow[slow_n++] = s;
		}

		if (slow_n > 0 &&
		    (k_uptime_get() - last_flush) >= SLOW_FLUSH_INTERVAL_MS) {
			tdms_write_slow(&s_tdms, slow, slow_n);
			slow_n = 0;
			last_flush = k_uptime_get();
			storage_sync(&s_file);
		}

		struct agg_window w;

		if (agg_take(&w)) {
			ble_stream_publish(&w);
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
	int rc;

	/* TODO: rolling session filename (counter / RTC) per REQUIREMENTS.md 4. */
	rc = storage_session_begin(&s_file, "log0001.tdms");
	if (rc != 0) {
		return rc;
	}

	rc = tdms_open(&s_tdms, &s_file);
	if (rc != 0) {
		storage_session_end(&s_file);
		return rc;
	}

	s_running = true;
	writer_tid = k_thread_create(&writer_thread, writer_stack,
				     K_THREAD_STACK_SIZEOF(writer_stack),
				     writer_fn, NULL, NULL, NULL,
				     WRITER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(writer_tid, "logger");

	acq_start();
	ina228_start();
	LOG_INF("logging started");
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

	tdms_close(&s_tdms);
	storage_session_end(&s_file);
	LOG_INF("logging stopped");
	return 0;
}
