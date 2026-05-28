#include "tdms.h"
#include "storage.h"
#include "app_config.h"
#include "clock.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
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

/* INA228 datasheet scaling constants (per code -> SI). VBUS_LSB and TEMP_LSB
 * are silicon fixed; VSHUNT_LSB depends on the ADCRANGE bit (we set ADCRANGE=1
 * in ina228.c -> 78.125 nV/code). CURRENT_LSB / POWER_LSB are programmable via
 * SHUNT_CAL; we set CURRENT_LSB = INA228_IMAX_A / 2^19 in firmware. By baking
 * these into NI Linear-Scale properties on each slow channel below, consumers
 * (npTDMS, LabVIEW, MATLAB) get SI values directly with no extra knowledge. */
#define INA228_VBUS_LSB_V    (195.3125e-6)
#define INA228_VSHUNT_LSB_V  (78.125e-9)
#define INA228_TEMP_LSB_C    (7.8125e-3)
#define INA228_CURRENT_LSB_A ((double)INA228_IMAX_A / 524288.0)
#define INA228_POWER_LSB_W   (3.2 * INA228_CURRENT_LSB_A)

/* Bumped from 1024 to 2048 -- the slow-channel full meta now carries 5
 * properties per channel (unit_string + 4 NI scale fields) instead of 1. */
static uint8_t s_meta[2048];
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

/* seconds between the NI/LabVIEW epoch (1904-01-01) and the Unix epoch */
#define NI_EPOCH_OFFSET 2082844800LL
#define NI_2POW64       18446744073709551616.0

/* master tick (1 us) -> NI timestamp, anchored to the session wall clock.
 * NI sec = (base_unix + (tick-base_tick)/master) + 1904 offset. When the clock
 * was never set (abs_ok=0) it degrades to seconds-since-session-start. */
static void ni_abs(const struct tdms_writer *w, uint64_t tick, uint64_t *frac, int64_t *sec)
{
	int64_t base = w->abs_ok ? (w->base_unix + NI_EPOCH_OFFSET) : 0;
	int64_t d = (int64_t)(tick - w->base_tick);   /* signed: a sample may pre-date the anchor */
	int64_t s = d / (int64_t)MASTER_CLOCK_HZ;
	int64_t rem = d % (int64_t)MASTER_CLOCK_HZ;

	if (rem < 0) {                 /* floor toward -inf so frac stays [0,1) */
		s -= 1;
		rem += MASTER_CLOCK_HZ;
	}
	*sec = base + s;
	*frac = (uint64_t)(((double)rem / (double)MASTER_CLOCK_HZ) * NI_2POW64);
}

/* Extend the slow channel's raw 32-bit capture ticks onto the 64-bit session
 * timeline (its own state, fed in capture order; a true wrap is a near-2^32
 * backward step, distinct from a few-ms pre-anchor sample). */
static uint64_t slow_extend(struct tdms_writer *w, uint32_t t32)
{
	if (t32 < w->slow_prev32 && (w->slow_prev32 - t32) > 0x80000000u) {
		w->slow_hi += (1ULL << 32);
	}
	w->slow_prev32 = t32;
	return w->slow_hi | (uint64_t)t32;
}

static void iso_utc(int64_t unix_s, char *buf, size_t cap)
{
	time_t t = (time_t)unix_s;
	struct tm tm;

	gmtime_r(&t, &tm);
	snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ---- metadata builders ------------------------------------------------- */
/* code -> amps. ADC_BITS=14 (hardware-oversampled), so full-scale code = 2^14. */
static const double FAST_SLOPE =
	(double)ADC_VFULL_SCALE_V /
	((double)(1u << ADC_BITS) * (double)INA240_GAIN * (double)INA240_SHUNT_OHMS);
static const double FAST_INTERCEPT =
	-(double)INA240_VREF_V / ((double)INA240_GAIN * (double)INA240_SHUNT_OHMS);

static size_t meta_fast_full(struct tdms_writer *w, uint8_t *out, size_t cap,
			     uint16_t n, uint64_t t_first)
{
	struct buf b = { out, cap, 0, false };
	uint64_t frac;
	int64_t sec;
	char iso[24];

	/* Establish the session wall-clock anchor on the first fast sample. */
	w->base_tick = t_first;
	w->base_unix = (int64_t)clk_now();
	w->abs_ok = clk_has_time();
	w->slow_hi = t_first & 0xFFFFFFFF00000000ULL;
	w->slow_prev32 = (uint32_t)t_first;
	w->have_anchor = true;
	if (w->abs_ok) {
		iso_utc(w->base_unix, iso, sizeof(iso));
	} else {
		strcpy(iso, "unsynced");
	}

	b_u32(&b, 3);   /* root + group + channel */

	/* root "/" : session/front-end properties for traceability. Format v2
	 * adds NI Linear Scale properties to each slow channel (so reads come
	 * out in SI) and exposes ina228_imax_a + ina228_current_lsb_a as
	 * documentation -- the scales themselves are on the channels. */
	b_str(&b, "/");
	b_u32(&b, NO_RAW_DATA);
	b_u32(&b, 12);
	p_i32(&b, "pwrlog_format_version", 2);
	p_i32(&b, "master_clock_hz", (int32_t)MASTER_CLOCK_HZ);
	p_f64(&b, "ina240_gain", INA240_GAIN);
	p_f64(&b, "ina240_vref_v", INA240_VREF_V);
	p_f64(&b, "ina240_shunt_ohms", INA240_SHUNT_OHMS);
	p_f64(&b, "ina228_shunt_ohms", INA228_SHUNT_OHMS);
	p_f64(&b, "ina228_imax_a", (double)INA228_IMAX_A);
	p_f64(&b, "ina228_current_lsb_a", INA228_CURRENT_LSB_A);
	p_f64(&b, "adc_vfull_scale_v", ADC_VFULL_SCALE_V);
	p_i32(&b, "adc_bits", (int32_t)ADC_BITS);
	p_str(&b, "session_start_utc", iso);
	p_i32(&b, "time_synced", clk_is_synced());

	/* group "/'fast'" */
	b_str(&b, "/'fast'");
	b_u32(&b, NO_RAW_DATA);
	b_u32(&b, 0);

	/* channel "/'fast'/'i'" : i16 raw codes, NI linear scale -> amps */
	ni_abs(w, t_first, &frac, &sec);
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

/* Emit a non-timestamp slow channel: path + raw-index + optional NI Linear
 * Scale (intercept=0) so the on-disk codes are auto-converted to SI on read.
 * When `with_props` is false this is a re-list (no metadata) -- scale
 * properties only carry on the first "full" emission per session. */
static void slow_scaled_channel(struct buf *b, const char *path, uint32_t dtype,
				size_t n, const char *unit, double slope, bool with_props)
{
	b_str(b, path);
	b_raw_index(b, dtype, n);
	b_u32(b, with_props ? 5 : 0);
	if (with_props) {
		p_str(b, "unit_string", unit);
		p_i32(b, "NI_Number_Of_Scales", 1);
		p_str(b, "NI_Scale[0]_Scale_Type", "Linear");
		p_f64(b, "NI_Scale[0]_Linear_Slope", slope);
		p_f64(b, "NI_Scale[0]_Linear_Y_Intercept", 0.0);
	}
}

static void slow_channels(struct buf *b, size_t n, bool with_props)
{
	/* channel order here defines the raw-data order in the segment */
	b_str(b, "/'slow'/'t'");
	b_raw_index(b, TDS_TIMESTAMP, n);
	b_u32(b, 0);

	slow_scaled_channel(b, "/'slow'/'vbus'",    TDS_I32, n, "V",   INA228_VBUS_LSB_V,    with_props);
	slow_scaled_channel(b, "/'slow'/'vshunt'",  TDS_I32, n, "V",   INA228_VSHUNT_LSB_V,  with_props);
	slow_scaled_channel(b, "/'slow'/'current'", TDS_I32, n, "A",   INA228_CURRENT_LSB_A, with_props);
	slow_scaled_channel(b, "/'slow'/'power'",   TDS_I32, n, "W",   INA228_POWER_LSB_W,   with_props);
	slow_scaled_channel(b, "/'slow'/'temp'",    TDS_I16, n, "degC", INA228_TEMP_LSB_C,   with_props);
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
static size_t slow_raw(struct tdms_writer *w, uint8_t *out, size_t cap,
		       const struct ina228_sample *s, size_t n)
{
	struct buf b = { out, cap, 0, false };

	for (size_t i = 0; i < n; i++) {
		uint64_t frac;
		int64_t sec;

		ni_abs(w, slow_extend(w, s[i].t_capture), &frac, &sec);
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
	w->have_anchor = false;
	w->abs_ok = false;
	w->base_unix = 0;
	w->base_tick = 0;
	w->slow_prev32 = 0;
	w->slow_hi = 0;
	w->marker_seq = 0;
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
		mlen = meta_fast_full(w, s_meta, sizeof(s_meta), n, t_first_tick);
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

	size_t rlen = slow_raw(w, s_slow, sizeof(s_slow), s, n);

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

/* Append a marker as a TDMS root property `marker_<N>` (UTC ISO timestamp +
 * the caller-supplied message). Emits a small metadata-only segment per
 * marker (~110 bytes); the receiving host's TDMS reader (npTDMS, LabVIEW,
 * DIAdem) merges root properties across segments, so the value shows up on
 * the file's root group as a normal property. Markers fired on the writer
 * thread; one per simultaneous REC+ROTATE button press. */
int tdms_write_event(struct tdms_writer *w, uint64_t tick, const char *msg)
{
	struct buf b = { s_meta, sizeof(s_meta), 0, false };
	char key[24];
	char val[48];

	w->marker_seq++;
	snprintf(key, sizeof(key), "marker_%u", (unsigned)w->marker_seq);

	if (w->have_anchor && w->abs_ok) {
		uint64_t frac;
		int64_t sec;
		char iso[24];
		ni_abs(w, tick, &frac, &sec);
		(void)frac;
		/* sec already includes the 1904 offset; subtract it for Unix. */
		iso_utc(sec - NI_EPOCH_OFFSET, iso, sizeof(iso));
		snprintf(val, sizeof(val), "%s %s", iso, msg ? msg : "");
	} else {
		snprintf(val, sizeof(val), "tick=%llu %s",
			 (unsigned long long)tick, msg ? msg : "");
	}

	b_u32(&b, 1);                /* one object: root */
	b_str(&b, "/");
	b_u32(&b, NO_RAW_DATA);
	b_u32(&b, 1);                /* one property added to root */
	p_str(&b, key, val);

	if (b.ovf) {
		LOG_ERR("tdms marker meta overflow");
		return -ENOMEM;
	}
	int rc = emit(w, TOC_META, s_meta, b.len, NULL, 0);
	if (rc == 0) {
		/* Marker meta-only segment doesn't disturb the fast/slow object
		 * set tracking; leave w->last_objset alone so the next fast or
		 * slow segment reuses its previous metadata correctly. */
		LOG_INF("tdms marker: %s = %s", key, val);
	} else {
		LOG_ERR("tdms marker emit failed: rc=%d", rc);
	}
	return rc;
}

int tdms_close(struct tdms_writer *w)
{
	ARG_UNUSED(w);
	/* Every segment's next_segment_offset is written correctly up front
	 * (sizes are known before writing), so no finalize pass is needed. */
	return 0;
}
