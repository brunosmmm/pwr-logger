#include "tdms.h"
#include "storage.h"
#include "app_config.h"

#include <string.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tdms, LOG_LEVEL_INF);

/*
 * NI TDMS segment encoder. See DATA_FORMAT.md sections 2-6.
 *
 * Segment = lead-in (28 B) + [metadata] + raw data:
 *   lead-in: "TDSm" | ToC u32 | version u32 (4713)
 *            | next_segment_offset u64 | raw_data_offset u64
 *   next_segment_offset = bytes after the lead-in (metadata + raw)
 *   raw_data_offset     = metadata length (0 when no metadata)
 *
 * Streaming: the first fast buffer carries full metadata (root props, fast
 * group, fast/i channel incl. NI linear scale + waveform timing); subsequent
 * fast buffers are raw-only (just the 28 B lead-in). Slow records are batched
 * and flushed ~1x/s as their own object set. Switching object sets (fast<->slow)
 * re-sends a (property-free) object list so the reader knows the raw layout, but
 * never re-sends wf_start_time (that would move the continuous waveform's t0).
 *
 * TDMS is little-endian (matches the nRF52840). Verified against npTDMS.
 */

/* ToC flags */
#define TOC_META    (1u << 1)
#define TOC_NEWOBJ  (1u << 2)
#define TOC_RAW     (1u << 3)

/* tdsDataType */
#define TDS_I16        2u
#define TDS_I32        3u
#define TDS_DOUBLE     10u
#define TDS_STRING     0x20u
#define TDS_TIMESTAMP  0x44u

#define TDMS_VERSION   4713u
#define NO_RAW_DATA    0xFFFFFFFFu

#define TDMS_SLOW_MAX  32u   /* max slow records per segment (logger batches <=16) */

/* one NI timestamp (16 B) + i32 vbus/vshunt/power + i16 temp, per record */
#define SLOW_REC_BYTES (16u + 4u + 4u + 4u + 4u + 2u) /* t,vbus,vshunt,current,power,temp */

static uint8_t s_meta[1024];
static uint8_t s_slow[TDMS_SLOW_MAX * SLOW_REC_BYTES];

/* ---- little-endian byte-buffer builder --------------------------------- */
struct buf {
	uint8_t *p;
	size_t   cap;
	size_t   len;
	bool     ovf;
};

static void b_bytes(struct buf *b, const void *d, size_t n)
{
	if (b->len + n > b->cap) {
		b->ovf = true;
		return;
	}
	memcpy(b->p + b->len, d, n);
	b->len += n;
}

static void b_u16(struct buf *b, uint16_t v)
{
	uint8_t t[2] = { v, v >> 8 };

	b_bytes(b, t, sizeof(t));
}

static void b_u32(struct buf *b, uint32_t v)
{
	uint8_t t[4] = { v, v >> 8, v >> 16, v >> 24 };

	b_bytes(b, t, sizeof(t));
}

static void b_u64(struct buf *b, uint64_t v)
{
	uint8_t t[8] = { v, v >> 8, v >> 16, v >> 24,
			 v >> 32, v >> 40, v >> 48, v >> 56 };

	b_bytes(b, t, sizeof(t));
}

static void b_f64(struct buf *b, double v)
{
	uint64_t u;

	memcpy(&u, &v, sizeof(u));   /* IEEE-754 LE on both target and host */
	b_u64(b, u);
}

static void b_str(struct buf *b, const char *s)
{
	uint32_t n = (uint32_t)strlen(s);

	b_u32(b, n);
	b_bytes(b, s, n);
}

/* raw data index for a numeric/timestamp channel: self-inclusive length 20,
 * then datatype, dimension(=1), value count (matches npTDMS's writer). */
static void b_raw_index(struct buf *b, uint32_t dtype, uint64_t count)
{
	b_u32(b, 20);
	b_u32(b, dtype);
	b_u32(b, 1);
	b_u64(b, count);
}

static void p_str(struct buf *b, const char *name, const char *val)
{
	b_str(b, name);
	b_u32(b, TDS_STRING);
	b_str(b, val);
}

static void p_i32(struct buf *b, const char *name, int32_t val)
{
	b_str(b, name);
	b_u32(b, TDS_I32);
	b_u32(b, (uint32_t)val);
}

static void p_f64(struct buf *b, const char *name, double val)
{
	b_str(b, name);
	b_u32(b, TDS_DOUBLE);
	b_f64(b, val);
}

static void p_time(struct buf *b, const char *name, uint64_t frac, int64_t sec)
{
	b_str(b, name);
	b_u32(b, TDS_TIMESTAMP);
	b_u64(b, frac);            /* fractions of a second (2^-64) first ... */
	b_u64(b, (uint64_t)sec);   /* ... then seconds since 1904-01-01 UTC   */
}

/* master tick (1 us) -> NI timestamp components. Relative mode: no host
 * wall-clock anchor yet, so this is seconds-since-boot off the 1904 epoch. */
static void tick_to_ni(uint64_t tick, uint64_t *frac, int64_t *sec)
{
	*sec = (int64_t)(tick / MASTER_CLOCK_HZ);
	uint64_t rem = tick % MASTER_CLOCK_HZ;   /* leftover 1 us ticks */

	/* frac = rem/MASTER_CLOCK_HZ of a second, scaled to 2^-64 units */
	*frac = (uint64_t)(((double)rem / (double)MASTER_CLOCK_HZ) * 18446744073709551616.0);
}

/* ---- metadata builders ------------------------------------------------- */
/* code -> amps. ADC_BITS=14 (hardware-oversampled), so full-scale code = 2^14. */
static const double FAST_SLOPE =
	(double)ADC_VFULL_SCALE_V /
	((double)(1u << ADC_BITS) * (double)INA240_GAIN * (double)INA240_SHUNT_OHMS);
static const double FAST_INTERCEPT =
	-(double)INA240_VREF_V / ((double)INA240_GAIN * (double)INA240_SHUNT_OHMS);

static size_t meta_fast_full(uint8_t *out, size_t cap, uint16_t n, uint64_t t_first)
{
	struct buf b = { out, cap, 0, false };
	uint64_t frac;
	int64_t sec;

	b_u32(&b, 3);   /* root + group + channel */

	/* root "/" : session/front-end properties for traceability */
	b_str(&b, "/");
	b_u32(&b, NO_RAW_DATA);
	b_u32(&b, 8);
	p_i32(&b, "pwrlog_format_version", 1);
	p_i32(&b, "master_clock_hz", (int32_t)MASTER_CLOCK_HZ);
	p_f64(&b, "ina240_gain", INA240_GAIN);
	p_f64(&b, "ina240_vref_v", INA240_VREF_V);
	p_f64(&b, "ina240_shunt_ohms", INA240_SHUNT_OHMS);
	p_f64(&b, "ina228_shunt_ohms", INA228_SHUNT_OHMS);
	p_f64(&b, "adc_vfull_scale_v", ADC_VFULL_SCALE_V);
	p_i32(&b, "adc_bits", (int32_t)ADC_BITS);

	/* group "/'fast'" */
	b_str(&b, "/'fast'");
	b_u32(&b, NO_RAW_DATA);
	b_u32(&b, 0);

	/* channel "/'fast'/'i'" : i16 raw codes, NI linear scale -> amps */
	tick_to_ni(t_first, &frac, &sec);
	b_str(&b, "/'fast'/'i'");
	b_raw_index(&b, TDS_I16, n);
	b_u32(&b, 8);
	p_str(&b, "unit_string", "A");
	p_i32(&b, "NI_Number_Of_Scales", 1);
	p_str(&b, "NI_Scale[0]_Scale_Type", "Linear");
	p_f64(&b, "NI_Scale[0]_Linear_Slope", FAST_SLOPE);
	p_f64(&b, "NI_Scale[0]_Linear_Y_Intercept", FAST_INTERCEPT);
	p_time(&b, "wf_start_time", frac, sec);
	p_f64(&b, "wf_increment", 1.0 / (double)ACQ_SAMPLE_RATE_HZ);
	p_f64(&b, "wf_start_offset", 0.0);

	return b.ovf ? 0 : b.len;
}

/* fast object re-list (after a slow segment): path + raw index, no properties
 * (scaling/waveform props persist from the first full segment). */
static size_t meta_fast_relist(uint8_t *out, size_t cap, uint16_t n)
{
	struct buf b = { out, cap, 0, false };

	b_u32(&b, 1);
	b_str(&b, "/'fast'/'i'");
	b_raw_index(&b, TDS_I16, n);
	b_u32(&b, 0);
	return b.ovf ? 0 : b.len;
}

static void slow_channels(struct buf *b, size_t n, bool with_props)
{
	/* channel order here defines the raw-data order in the segment */
	b_str(b, "/'slow'/'t'");
	b_raw_index(b, TDS_TIMESTAMP, n);
	b_u32(b, 0);

	b_str(b, "/'slow'/'vbus'");
	b_raw_index(b, TDS_I32, n);
	b_u32(b, with_props ? 1 : 0);
	if (with_props) {
		p_str(b, "unit_string", "V_raw");
	}

	b_str(b, "/'slow'/'vshunt'");
	b_raw_index(b, TDS_I32, n);
	b_u32(b, with_props ? 1 : 0);
	if (with_props) {
		p_str(b, "unit_string", "V_raw");
	}

	b_str(b, "/'slow'/'current'");
	b_raw_index(b, TDS_I32, n);
	b_u32(b, with_props ? 1 : 0);
	if (with_props) {
		p_str(b, "unit_string", "A_raw");
	}

	b_str(b, "/'slow'/'power'");
	b_raw_index(b, TDS_I32, n);
	b_u32(b, with_props ? 1 : 0);
	if (with_props) {
		p_str(b, "unit_string", "W_raw");
	}

	b_str(b, "/'slow'/'temp'");
	b_raw_index(b, TDS_I16, n);
	b_u32(b, with_props ? 1 : 0);
	if (with_props) {
		p_str(b, "unit_string", "degC_raw");
	}
}

static size_t meta_slow(uint8_t *out, size_t cap, size_t n, bool full)
{
	struct buf b = { out, cap, 0, false };

	b_u32(&b, full ? 7 : 6);   /* group + 6 channels, or 6 channels on re-list */
	if (full) {
		b_str(&b, "/'slow'");
		b_u32(&b, NO_RAW_DATA);
		b_u32(&b, 0);
	}
	slow_channels(&b, n, full);
	return b.ovf ? 0 : b.len;
}

/* de-interleave slow records into channel-contiguous raw data */
static size_t slow_raw(uint8_t *out, size_t cap, const struct ina228_sample *s, size_t n)
{
	struct buf b = { out, cap, 0, false };

	for (size_t i = 0; i < n; i++) {
		uint64_t frac;
		int64_t sec;

		tick_to_ni(s[i].t_capture, &frac, &sec);
		b_u64(&b, frac);
		b_u64(&b, (uint64_t)sec);
	}
	for (size_t i = 0; i < n; i++) {
		b_u32(&b, (uint32_t)s[i].vbus_raw);
	}
	for (size_t i = 0; i < n; i++) {
		b_u32(&b, (uint32_t)s[i].vshunt_raw);
	}
	for (size_t i = 0; i < n; i++) {
		b_u32(&b, (uint32_t)s[i].current_raw);
	}
	for (size_t i = 0; i < n; i++) {
		b_u32(&b, (uint32_t)s[i].power_raw);
	}
	for (size_t i = 0; i < n; i++) {
		b_u16(&b, (uint16_t)s[i].temp_raw);
	}
	return b.ovf ? 0 : b.len;
}

/* ---- segment emit ------------------------------------------------------ */
static int emit(struct tdms_writer *w, uint32_t toc,
		const uint8_t *meta, size_t meta_len,
		const void *raw, size_t raw_len)
{
	uint8_t lead[28];
	struct buf lb = { lead, sizeof(lead), 0, false };
	int rc;

	b_bytes(&lb, "TDSm", 4);
	b_u32(&lb, toc);
	b_u32(&lb, TDMS_VERSION);
	b_u64(&lb, (uint64_t)(meta_len + raw_len));   /* next segment offset */
	b_u64(&lb, (uint64_t)meta_len);               /* raw data offset     */

	rc = storage_write(w->file, lead, sizeof(lead));
	if (rc) {
		return rc;
	}
	if (meta_len) {
		rc = storage_write(w->file, meta, meta_len);
		if (rc) {
			return rc;
		}
	}
	if (raw_len) {
		rc = storage_write(w->file, raw, raw_len);
		if (rc) {
			return rc;
		}
	}
	return 0;
}

/* ---- public API -------------------------------------------------------- */
int tdms_open(struct tdms_writer *w, struct storage_file *f)
{
	w->file = f;
	w->fast_meta = false;
	w->slow_meta = false;
	w->last_objset = 0;
	w->fast_seq = 0;
	return 0;
}

int tdms_write_fast(struct tdms_writer *w, const int16_t *samples,
		    uint16_t n, uint64_t t_first_tick)
{
	size_t mlen = 0;
	uint32_t toc;

	if (w->last_objset == 1) {
		toc = TOC_RAW;                       /* raw-only: reuse prev list */
	} else if (!w->fast_meta) {
		mlen = meta_fast_full(s_meta, sizeof(s_meta), n, t_first_tick);
		w->fast_meta = true;
		toc = TOC_META | TOC_NEWOBJ | TOC_RAW;
	} else {
		mlen = meta_fast_relist(s_meta, sizeof(s_meta), n);
		toc = TOC_META | TOC_NEWOBJ | TOC_RAW;
	}
	if (toc & TOC_META && mlen == 0) {
		LOG_ERR("fast metadata overflow");
		return -ENOMEM;
	}

	int rc = emit(w, toc, s_meta, mlen, samples, (size_t)n * sizeof(int16_t));

	if (rc == 0) {
		w->last_objset = 1;
		w->fast_seq++;
	}
	return rc;
}

int tdms_write_slow(struct tdms_writer *w, const struct ina228_sample *s, size_t n)
{
	if (n == 0) {
		return 0;
	}
	if (n > TDMS_SLOW_MAX) {
		LOG_WRN("slow batch %zu > %u, truncating", n, TDMS_SLOW_MAX);
		n = TDMS_SLOW_MAX;
	}

	size_t mlen;
	uint32_t toc;

	if (w->last_objset == 2) {
		mlen = 0;
		toc = TOC_RAW;
	} else {
		bool full = !w->slow_meta;

		mlen = meta_slow(s_meta, sizeof(s_meta), n, full);
		w->slow_meta = true;
		toc = TOC_META | TOC_NEWOBJ | TOC_RAW;
	}
	if (toc & TOC_META && mlen == 0) {
		LOG_ERR("slow metadata overflow");
		return -ENOMEM;
	}

	size_t rlen = slow_raw(s_slow, sizeof(s_slow), s, n);

	if (rlen == 0) {
		LOG_ERR("slow raw overflow");
		return -ENOMEM;
	}

	int rc = emit(w, toc, s_meta, mlen, s_slow, rlen);

	if (rc == 0) {
		w->last_objset = 2;
	}
	return rc;
}

int tdms_write_event(struct tdms_writer *w, uint64_t tick, const char *msg)
{
	ARG_UNUSED(w);
	ARG_UNUSED(tick);
	LOG_INF("tdms event: %s", msg ? msg : "");
	/* TODO: write to /'events'/'marker' (string channel). */
	return 0;
}

int tdms_close(struct tdms_writer *w)
{
	ARG_UNUSED(w);
	/* Every segment's next_segment_offset is written correctly up front
	 * (sizes are known before writing), so no finalize pass is needed. */
	return 0;
}
