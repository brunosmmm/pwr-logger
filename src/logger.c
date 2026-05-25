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
static bool s_session; /* true = recording to SD; false = monitor-only */

#define DECIM_OUT_MAX (ACQ_BUF_SAMPLES / DECIM_FACTOR + 2)

static struct {
	int32_t  acc;
	uint16_t cnt;
} s_decim;
static int16_t s_decim_out[DECIM_OUT_MAX];

/*
 * Oversample decimation: average DECIM_FACTOR raw codes into one output sample,
 * keeping DECIM_SHIFT sub-LSB bits so the averaging's resolution gain survives.
 * Carries across DMA buffers so the logged 20 kSPS stream stays gapless. Returns
 * the number of output samples produced for this input block.
 */
static size_t decimate(const int16_t *in, size_t n, int16_t *out)
{
	size_t k = 0;

	for (size_t i = 0; i < n; i++) {
		s_decim.acc += in[i];
		if (++s_decim.cnt == DECIM_FACTOR) {
			out[k++] = (int16_t)((s_decim.acc << DECIM_SHIFT) / DECIM_FACTOR);
			s_decim.acc = 0;
			s_decim.cnt = 0;
		}
	}
	return k;
}

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

	while (s_running) {
		struct acq_block blk;

		if (k_msgq_get(&acq_msgq, &blk, K_MSEC(100)) == 0) {
			uint64_t t = timebase_extend(blk.t_capture);

			agg_push(blk.samples, blk.count);           /* always: live view on raw */
			if (s_session) {
				size_t dn = decimate(blk.samples, blk.count, s_decim_out);

				if (dn) {
					tdms_write_fast(&s_tdms, s_decim_out, dn, t);
				}
			}
			samples += blk.count;
			acq_block_release(&blk);                    /* always */
		}

		struct ina228_sample s;

		while (slow_n < SLOW_BATCH_MAX &&
		       k_msgq_get(&ina228_msgq, &s, K_NO_WAIT) == 0) {
			slow[slow_n++] = s;
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
			LOG_INF("acq: %llu samples/s, %u drops [%s]", samples,
				acq_drops(), s_session ? "logging" : "monitor");
			samples = 0;
			t_report = now;
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
	/* SD session is OPTIONAL: with no card we still acquire (monitor mode). */
	s_session = false;
	s_decim.acc = 0;
	s_decim.cnt = 0;
	if (storage_mount() == 0 &&
	    storage_session_begin(&s_file, "log0001.tdms") == 0 &&
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
