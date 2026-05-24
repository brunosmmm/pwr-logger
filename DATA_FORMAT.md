# pwr-logger — Time Base & Data Format

How the fast (SAADC, 200 kSPS) and slow (INA228, configurable) streams are
timestamped against a common clock and stored as **NI TDMS**, an openly
documented, streaming-friendly format with open-source read/write tooling
(npTDMS for Python, plus MATLAB/.NET/C++ readers). The format sits **above** the
storage seam (`storage.[ch]`). TDMS is **little-endian**, which matches us.

## 1. Time base

Two acquisition domains, independent clocks:
- SAADC sample timer → HFCLK (32 MHz crystal, ±~20–40 ppm) — accurate, exact.
- INA228 conversions → its own internal oscillator — drifts vs. the MCU.

**Solution: one free-running HFCLK `TIMER` @ 1 MHz (1 µs ticks) is the master
clock; both streams are hardware-stamped against it via PPI (no ISR jitter).**

- **Fast:** `SAADC.END` → PPI → `TIMER.CAPTURE[n]` latches the master tick at
  each DMA-buffer boundary. Sample *i* in the block is `anchor + i × period`.
  Trigger and master share HFCLK ⇒ period is an **exact integer** (200 kHz from
  1 MHz = 5 ticks); reconstruction is integer-only and drift-free.
- **Slow:** INA228 `CNVR` pin → GPIOTE → PPI → `TIMER.CAPTURE[m]` latches a tick
  the instant each conversion completes — the INA228's drift is irrelevant.
- **64-bit extension:** TIMER is 32-bit (wraps ~71.6 min). The SD-writer thread
  extends to 64-bit in software by detecting a captured value < previous (ticks
  arrive every ~41 ms ≪ wrap, so no wrap ISR needed).

**Absolute time:** the session anchor `(wallclock_unix_us, t0_master_tick)` is
set from the BLE/USB host at session start if available (else relative-only).
Wall-clock of any sample = `wallclock_unix_us + (tick − t0_master_tick)`. This
anchor is what we convert into TDMS NI timestamps (see §5).

## 2. TDMS overview & object layout

A TDMS file is a sequence of **segments** (lead-in + optional metadata + raw
data). One segment ≈ one of our DMA buffers, so streaming maps directly onto the
SD-writer flow. Object hierarchy we use:

```
/                              file (root) properties: pwrlog_format_version,
                               session_uuid, anchor, INA240 gain/Vref, shunt Ω,
                               INA228 calibration, master_clock_hz
/'fast'                        group
/'fast'/'i'                    I16 channel — raw SAADC codes (waveform; see §5)
/'slow'                        group
/'slow'/'t'                    Timestamp channel (per-sample CNVR capture)
/'slow'/'vbus'                 I32 (24-bit sign-extended)
/'slow'/'vshunt'               I32
/'slow'/'power'                I32
/'slow'/'temp'                 I16
/'events'/'marker'             (optional) user markers / gap notices
```

Fast stream is a **uniform waveform** (one continuous channel). Slow stream is
**irregular**, so it carries an explicit per-sample timestamp channel rather than
a waveform increment.

## 3. Segment structure & streaming use

Lead-in (28 B): `"TDSm"` + ToC mask (u32) + version (u32, **4713**) +
next-segment-offset (u64) + raw-data-offset (u64).

ToC flags we use: `kTocMetaData=1<<1`, `kTocNewObjList=1<<2`, `kTocRawData=1<<3`
(little-endian; `kTocBigEndian=1<<6` stays clear). *(Verify exact values against
the NI spec when implementing.)*

Streaming pattern (minimizes overhead):
- **First segment** of a channel-set: full metadata — object list + properties +
  raw-data index. (`kTocMetaData | kTocNewObjList | kTocRawData`.)
- **Subsequent same-set segments** (e.g. a run of fast buffers): lead-in + raw
  data only, raw-data index = "matches previous" (`0x00000000`), no new object
  list → per-segment overhead is just the 28 B lead-in.
- **Fast/slow alternation:** slow records are accumulated in RAM and flushed as a
  slow segment ~1×/s (or every K records), so the metadata re-send at each
  fast↔slow transition is negligible.
- Each segment's size is known before writing (fixed DMA buffer / batched slow
  records), so next-segment-offset is written correctly up front.

## 4. Self-describing scaling (raw → physical, standard mechanism)

Use TDMS **native linear scaling** properties (auto-applied by npTDMS/DIAdem) so
readers return physical units directly — no external knowledge:

```
NI_Number_Of_Scales            = 1            (I32)
NI_Scale[0]_Scale_Type         = "Linear"     (String)
NI_Scale[0]_Linear_Slope       = <DBL>
NI_Scale[0]_Linear_Y_Intercept = <DBL>
```
Fast current: `I = slope·code + intercept`, with
`slope = Vfull / (2^bits · gain · shunt)`, `intercept = −Vref / (gain · shunt)`.
Slow channels: per-register linear scaling (e.g. `vbus_lsb`, `current_lsb`).
Also set `unit_string` ("A", "V", "W", "degC") per channel. The raw constants
themselves are duplicated as root properties for traceability.

## 5. Timestamps in TDMS

- **NI timestamp type** (`0x44`, 16 B): `uint64` fractions-of-second (2⁻⁶⁴) +
  `int64` seconds since the **NI epoch 1904-01-01 00:00:00 UTC**. *(Confirm field
  order against the spec — npTDMS reads fractions first, then seconds.)*
- **Fast channel** = waveform: set `wf_start_time` (timestamp of first sample,
  from the anchor), `wf_increment` = 5e-6 (DBL), `wf_start_offset` = 0. npTDMS
  then exposes a continuous time track. *Assumes no gaps* — see §6.
- **Slow channel** `/'slow'/'t'`: one NI timestamp per sample, from each CNVR
  capture → correct even though the INA228 is asynchronous.
- With no host wall-clock, anchor to session start (relative); timestamps are
  still internally consistent.

## 6. Robustness (power loss & gaps)

- **Truncated final segment:** if power is lost mid-segment, the last lead-in may
  claim more than exists; npTDMS detects and drops/partial-reads the incomplete
  final segment. Writing next-segment-offset = `0xFFFFFFFFFFFFFFFF` while a
  segment is in flight (patched to the real size after the data is `f_sync`'d) is
  the safe "extends-to-EOF" convention.
- **Dropped fast buffer (pool overrun):** breaks the gapless-waveform assumption.
  Mitigate by sizing the ring buffer to never overrun; if one occurs, emit an
  `events` marker and **start a new fast waveform segment with a fresh
  `wf_start_time`** so downstream timing stays correct across the gap.
- `f_sync()` cadence per `REQUIREMENTS.md` §4; file rolling still applies (each
  rolled file is a complete standalone TDMS).

## 7. BLE live schema (separate, smaller)

The live view is **derived** from the same buffers, not TDMS segments: decimated
waveform preview + per-window RMS / avg / min / max + status/alarms, in a compact
GATT characteristic layout (TBD). It shares the scaling constants so the host
shows physical units. Full fidelity stays on SD.

## 8. Decisions & open items

- **TDMS** chosen for: open documented format, open-source read/write (npTDMS),
  segment-per-buffer streaming, power-loss tolerance, native multi-rate +
  timestamps + linear scaling.
- **i16 raw** fast codes (scaling via TDMS linear props, not pre-converted) — DMA
  buffers are already i16; keeps MCU CPU ~0.
- Reference **host reader = npTDMS** (Python); validate output early.
- **Open:** exact byte-level encoding to verify against the NI spec (ToC values,
  timestamp field order, raw-data-index layout); INA228 rate/averaging config;
  slow-flush cadence; BLE GATT characteristic layout.
