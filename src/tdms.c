#include "tdms.h"
#include "storage.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tdms, LOG_LEVEL_INF);

/*
 * NI TDMS segment encoder. See DATA_FORMAT.md sections 2-6.
 *
 * Segment = lead-in (28 B) + [metadata] + raw data:
 *   lead-in: "TDSm" | ToC u32 | version u32 (4713)
 *            | next_segment_offset u64 | raw_data_offset u64
 *   ToC bits: kTocMetaData(1<<1) kTocNewObjList(1<<2) kTocRawData(1<<3)
 *
 * Streaming plan:
 *   - first fast segment: full metadata (objects + properties + raw index),
 *     incl. NI linear-scale props and wf_start_time / wf_increment(=5e-6).
 *   - subsequent same-set segments: lead-in + raw data only (index "matches
 *     previous" = 0x00000000), so per-segment overhead is just 28 B.
 *   - slow segments (batched ~1x/s) carry the slow-group channels incl. an
 *     explicit NI-timestamp channel.
 *   - on a dropped fast buffer: write an EVENT + start a fresh waveform segment
 *     with a new wf_start_time.
 *
 * !!! Verify exact ToC values, timestamp field order, and raw-data-index layout
 *     against the NI spec; add an npTDMS round-trip test early. !!!
 */

int tdms_open(struct tdms_writer *w, struct storage_file *f)
{
	w->file = f;
	w->header_written = false;
	w->last_objset = 0;
	w->fast_seq = 0;
	LOG_WRN("tdms_open: TODO emit file/root + channel metadata on first segment");
	return 0;
}

int tdms_write_fast(struct tdms_writer *w, const int16_t *samples,
		    uint16_t n, uint64_t t_first_tick)
{
	ARG_UNUSED(samples);
	ARG_UNUSED(n);
	ARG_UNUSED(t_first_tick);
	w->fast_seq++;
	/* TODO: build lead-in (+ metadata if first), append raw i16 via storage_write */
	return 0;
}

int tdms_write_slow(struct tdms_writer *w, const struct ina228_sample *s,
		    size_t n)
{
	ARG_UNUSED(w);
	ARG_UNUSED(s);
	ARG_UNUSED(n);
	/* TODO: slow segment: timestamp channel + vbus/vshunt/power/temp channels */
	return 0;
}

int tdms_write_event(struct tdms_writer *w, uint64_t tick, const char *msg)
{
	ARG_UNUSED(w);
	ARG_UNUSED(tick);
	LOG_INF("tdms event: %s", msg ? msg : "");
	/* TODO: write to /'events'/'marker' */
	return 0;
}

int tdms_close(struct tdms_writer *w)
{
	ARG_UNUSED(w);
	/* TODO: finalize last segment's next_segment_offset if using the
	 * 0xFFFFFFFFFFFFFFFF "extends-to-EOF" convention. */
	return 0;
}
