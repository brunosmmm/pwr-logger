# pwr-logger — Requirements & Architecture

A voltage / current / power logger built around precision power-monitor ICs
(INA228, optionally INA226) plus a high-speed analog channel sampled directly by
the MCU ADC. First build targets a **single channel** for simplicity; the
architecture is intended to scale to multiple channels later.

> Status: requirements defined, MCU selected. Pre-implementation.

---

## 1. MCU selection

**Selected: Nordic nRF52840** (Cortex-M4F @ 64 MHz, FPU, 1 MB flash, 256 KB RAM,
BLE 5, native USB Full-Speed, QSPI).

### Why, vs. the other candidates on hand

| MCU | Verdict |
|---|---|
| **nRF52840** | **Chosen.** Best/cleanest built-in ADC, FPU for power math, DMA+PPI pipeline makes high-rate acquisition essentially CPU-free, best-in-class OTA (BLE *and* USB), BLE gives the most practical wireless bandwidth with zero infrastructure. |
| ESP32-S3 | Only contender that can stream *full-rate* over WiFi, but noisier ADC and worse determinism. Rejected once an aggregated live view was accepted (no need for full-rate streaming). |
| RP2040 | Fastest raw ADC (500 kSPS) but documented linearity/DNL issues, no FPU (software float for RMS/power math), and no native radio. |
| ESP32-C6 | Good radio mix (WiFi 6 / BLE / 802.15.4) but weak, noisy ADC; SD over SPI only. |
| ESP32-H2 | BLE + 802.15.4 only, lowest bandwidth/clock. Poor fit. |

### Why the 64 MHz clock is not a bottleneck

The acquisition path is **DMA-driven, not CPU-driven**:

```
TIMER (sample clock)
  └─PPI──► SAADC.SAMPLE ──EasyDMA──► RAM buffer ──END──► ping-pong to 2nd buffer
                                                          │
                                                          └─► ring buffer ─► SD writer
                                                                            └─► BLE (decimated)
```

The CPU only wakes **once per buffer**, not per sample. At 200 kSPS with a
256-sample buffer that's one interrupt every ~1.28 ms — the M4F is idle ~99% of
the time, leaving ample headroom for the SD filesystem, BLE stack, and power
math. PPI triggers sampling in hardware (jitter-free), and the FPU handles
RMS/power calculations.

---

## 2. Channel architecture (single-channel first build)

Two complementary sensors on the **same rail**:

### High-speed current channel — INA240 → MCU ADC
- **INA240** current-sense amplifier *is* the analog front-end (no extra op-amp).
  Fixed-gain (A1=20, A2=50, A3=100, A4=200 V/V); low output impedance drives the
  SAADC directly; common-mode −4 V…+80 V (high-side capable); enhanced PWM
  rejection.
- **Gain variant:** pick A1/A2 — their bandwidth comfortably clears the 100 kHz
  Nyquist; the higher-gain parts have less headroom.
- **Anti-aliasing (mandatory):** RC low-pass at ~80–100 kHz between INA240 and
  the ADC pin (the amp passes content to ~400 kHz, which would fold back). Keep
  series R ≤ ~10 kΩ to stay within the SAADC's 3 µs acquisition budget — trivial
  given the INA240's low output-Z.

### Precision channel — INA228 over I²C
- **INA228** (20-bit ΔΣ) on the same rail provides lab-grade slow **V, I, P,
  energy, charge, temperature**. This is where bus voltage and accurate
  averages/energy integration come from (the INA240 is current-only).
- I²C at Fast-mode-plus (1 MHz). Practical ceiling ~5–7 kSPS for V+I+P
  (I²C readout overhead, not the chip's ~10 kSPS convert rate, is the limit).
- Use the conversion-ready (ALERT/CNVR) pin to pace reads.

### Future
- Additional channels via more INA228 / INA226 over I²C, and/or more SAADC
  channels (SAADC multiplexes — N channels share the 200 kSPS aggregate, with
  ~5 µs inter-channel skew).

---

## 3. Sampling & data rates

| Path | Rate | Resolution | Notes |
|---|---|---|---|
| INA240 → SAADC (fast current) | **200 kSPS** | 12-bit raw, ~10 ENOB | Max rate ⟹ **no oversampling**. Oversampling for cleaner ~14-bit only at lower rates. |
| INA228 → I²C (precision V/I/P) | ~5–7 kSPS | 20-bit ΔΣ | Slow, precise; energy/charge integration on-chip. |

**Fast-channel data volume:** 200 kSPS × 2 B = **400 kB/s ≈ 3.2 Mbps**.

---

## 4. Storage (SD card)

- **Full-rate logging to SD**, decoupled from the wireless live view. SD captures
  every sample; BLE carries only a decimated/aggregated view.
- Interface: **SPI on SPIM3** (32 MHz; other SPIM instances cap at 8 MHz),
  EasyDMA-driven. 400 kB/s is well within SPI-SD capability.
- **The risk is card latency, not bandwidth.** Cards can stall 50–100 ms for
  internal housekeeping → absorb with a **RAM ring buffer ≥ 128 KB** (~320 ms of
  cushion at full rate; 256 KB total RAM leaves room).
- **Storage backend is abstracted** behind a thin `storage` seam so the
  filesystem-vs-raw decision can be deferred at ~zero cost. With `f_expand()`
  contiguous files, the FAT and raw-sector *hot write paths are identical*
  (append aligned blocks into a pre-allocated extent); they diverge only at
  mount and offload. The on-card record format sits *above* this seam, so it is
  the same either way.
  ```
  storage_mount()
  storage_session_begin(meta) -> handle   // f_expand file | reserve sector range
  storage_write(handle, buf, len)         // append aligned blocks (shared path)
  storage_sync(handle)                    // flush FAT/dir  | update superblock
  storage_session_end(handle)
  storage_session_list() / storage_session_read(id, off, len)   // offload
  ```
- **Default backend: exFAT** (FatFs `FF_FS_EXFAT`), one contiguous (`f_expand`)
  file per session + file rolling. Chosen over FAT32 to drop the 4 GB file-size
  limit (hit in ~3 h at full rate) and for larger clusters → fewer FAT-table
  updates. Natively readable on Windows/macOS/Linux (kernel ≥ 5.4). Keeps every
  offload path open (USB-MSC, eject-and-read, *and* host tool). At 400 kB/s this
  is almost certainly fast enough; raw-sector determinism only matters at
  multi-MB/s or under hard real-time. (FAT32 is the fallback only for an old/
  embedded reader. exFAT carries a Microsoft patent — opened to Linux/OIN in
  2019; non-issue for personal/research, glance-worthy if commercialized.)
- **Raw-sector backend is the documented fallback, gated on a measurement:** if
  at the target rate (or when scaling channels/rate) worst-case write+sync
  latency exceeds the ring-buffer drain budget and causes overruns that bigger
  buffers + `f_expand` can't fix, drop in the raw backend (pairs with USB-CDC/BLE
  offload, not MSC — Windows treats a raw card as "unformatted"). A single giant
  `f_expand` container with an internal segment index is the middle ground if
  raw-like determinism *and* mountability are ever both needed.
- **Write strategy (matters more than FAT32 vs exFAT):**
  - **Pre-allocate contiguous files with `f_expand()`** and write in
    cluster/sector-aligned multiples — avoids per-write FAT-table updates, the
    main source of mid-stream latency spikes.
  - **File rolling regardless** (size- or time-based) — for crash robustness and
    manageable offload chunks, *not* the size limit. FAT/exFAT aren't journaled,
    so rolling caps power-loss/corruption blast radius to one segment.
  - **`f_sync()` periodically** (every few s / N MB) to flush metadata; the ring
    buffer absorbs the flush stalls.
  - Format with a **large cluster size**; use multi-block writes.
- **Card:** at ~1.44 GB/hour continuous, spec a **high-endurance card**
  (dashcam/CCTV class, pSLC/MLC) with a good *sustained* sequential-write rating.

---

## 5. Wireless (BLE only)

- **BLE 5, 2M PHY**, with DLE + large MTU (247) + fast connection interval →
  **~1.0–1.4 Mbps** practical application throughput. Zero infrastructure
  (stream straight to phone/laptop; Web Bluetooth viable).
- *Thread/802.15.4 explicitly rejected:* despite being a candidate, its practical
  goodput (~50–150 kbps single-hop) is ~10× lower than BLE and it requires a
  border router. Only worth it for a fixed mesh of many loggers — not this
  instrument.
- **Live view is aggregated/decimated** (accepted requirement): decimated
  waveform preview + RMS / avg / min / max windows + status + control/alarms.
  Full-rate streaming is *not* a requirement — the full record lives on SD.
  - 200 kSPS full-rate (3.2 Mbps) exceeds BLE and is intentionally not streamed.
  - INA228 precision path (~0.5 Mbps) *could* stream live in full if desired.

---

## 6. Firmware update & data offload (both wireless and USB)

- **OTA firmware update:** MCUboot + **mcumgr SMP** over **BLE** (`mcumgr` CLI /
  Device Manager app), image written to the internal slot1 partition. Also over
  **USB** (mcumgr SMP / USB-DFU). The stock UF2 bootloader is replaced by MCUboot.
- **SD offload:**
  - *USB:* expose the card as **USB Mass Storage** (USB FS ≈ ~1 MB/s practical).
  - *Wireless:* pull files over BLE (slow), or just eject the card.
  - **Mode-switched, mutually exclusive:** the MCU's FATFS and a USB host must
    never write the card simultaneously (filesystem corruption). Firmware state
    machine: **logging mode** (MCU owns card) ↔ **offload mode** (host owns card,
    logging paused). Bake this in from the start.

---

## 7. Key design constraints / gotchas (consolidated)

- SAADC 200 kSPS ⟹ single channel, 12-bit, **no oversampling**; 3 µs acquisition
  ⟹ source R ≤ ~10 kΩ.
- INA240 is **current-only** — voltage/power come from the INA228.
- **Anti-alias filter required** on the fast channel (~80–100 kHz).
- SD: **ring buffer for latency**, **file rolling / exFAT** for size.
- USB and on-board logging to the card are **mutually exclusive**.
- Keep buffer-swap ISR minimal (flip pointer + post to task); the BLE controller
  owns hard-real-time radio interrupts.

---

## 8. Open / future items

- Multi-channel scaling (more INA228/INA226 over I²C; more SAADC channels).
- Higher-resolution mode (SAADC oversampling at reduced rate) as a selectable
  trade-off.
- On-device DSP on the fast channel (FFT, real-time RMS) — FPU is available.
- Host-side / phone app for the BLE live view and control.
- Time base / timestamping strategy for correlating fast (SAADC) and slow
  (INA228) streams.
