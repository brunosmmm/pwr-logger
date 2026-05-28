# pwr-logger

Voltage / current / power logger on the **ProMicro nRF52840**, built with **upstream Zephyr 4.4** (fully open source — no proprietary blobs, no Nordic SDK).

Two acquisition channels, on-board SD recording in **NI TDMS**, **BLE** live telemetry, **USB MSC** offload, a **custom vendor-class bulk endpoint** for high-throughput downloads, an OLED status UI with momentary controls. Boots, records, recovers, downloads cleanly.

## Channels

| | Front-end | ADC | Rate | Purpose |
|---|---|---|---|---|
| **Fast** | INA240 shunt amplifier | nRF52840 SAADC, 14-bit | 20 kSPS (HW oversample 8×) | Transients, RMS, FFT |
| **Precision** | INA228 (I²C, hardware-timestamped) | 24-bit ΔΣ on-chip | ~20 Hz | Calibrated V / I / P / energy / temp |

Both streams co-recorded into the same TDMS file, hardware-timestamped from a shared 1 MHz master tick.

## Storage / transport

- **SD card**: FatFs/exFAT, full-rate streaming. Each session is one TDMS file. Verified zero drops at sustained 20 kSPS.
- **BLE**: aggregated 20 Hz telemetry stream + control characteristic. Service UUID `f0a00000-1234-5678-9abc-def012345678`. Zephyr's open SW link-layer controller (no blob).
- **USB MSC**: mount the card as a mass-storage device for ad-hoc retrieval.
- **Vendor-class bulk download**: `pwrdl.py` reads a file by name without mount/unmount dance. **~560 KB/s** sustained on USB Full-Speed (~56% of bulk ceiling). See `tools/pwrdl.py`.

## On-glass UI + controls

128×64 SSD1306 OLED + three buttons:

- **REC** (P0.10): short = start/stop logging, long = rotate TDMS file.
- **ROTATE** (P0.17): short = next UI page, long = clear HOLD on current channel.
- **OFFLOAD** (P0.20): mode switch (logging ↔ MSC offload).

Multi-page status UI: live, rolling stats, peaks, hold, energy, file, system. Auto-blanks on idle; wakes on any button.

## Status

**Verified working.** Each of these has hard evidence in `VERIFY_RESULTS.md`:

- INA228 V / I / P calibrated against bench supply (INA228 is the precision reference; bench supply over-reads ~4–5%).
- Fast SAADC current channel — INA240 VREF calibrated to 1.6622 V, gain/offset verified.
- SD round-trip — TDMS files parse via npTDMS, mean current matches supply, 0 drops.
- BLE — service advertises as "pwr-logger", telemetry NOTIFY at ~20 Hz consumed by a Python bleak client.

## Build & flash

Build is a freestanding app linked against an existing upstream Zephyr 4.4 workspace.

```sh
cd ~/work/sensor-station   # or wherever the Zephyr workspace lives
~/work/sensor-station/.venv/bin/west build \
    -s ~/work/pwr-logger \
    -b promicro_nrf52840/nrf52840 \
    -d /tmp/pwrlog-prod \
    --sysbuild \
    -- -DCONFIG_APP_BRINGUP=n
```

Artifacts:
- `/tmp/pwrlog-prod/mcuboot/zephyr/zephyr.hex` — MCUboot
- `/tmp/pwrlog-prod/pwr-logger/zephyr/zephyr.signed.hex` — signed app

**Flash** with the Raspberry Pi Debug Probe via pyocd (MCUboot already on the board → app-only is fine):

```sh
pyocd flash -t nrf52840 -O smart_flash=false \
    /tmp/pwrlog-prod/pwr-logger/zephyr/zephyr.signed.hex
pyocd reset -t nrf52840
pkill -x pyocd                # always; never leave the probe held
```

After a chip erase, also flash the MCUboot hex first.

SWD pads are on the underside of the ProMicro — solder fly-leads.

## Operate

Plug USB. The device enumerates as a composite with VID:PID `2fe3:0006`:

- **CDC ACM shell** on `/dev/ttyACM*` (look for `ID_MODEL=pwr-logger`) — `pwr status`, `pwr ls`, `pwr config ...`, `pwr download <file>`, etc.
- **USB MSC** — exposes the SD card as a block device (`PWRLOG` partition). Mutually exclusive with logging — use `pwr offload` / `pwr resume`.
- **Vendor bulk-IN** (interface 3, EP 0x84) — high-throughput download channel for `tools/pwrdl.py`.

For non-root access to the vendor interface on Linux, install the udev rule once:

```sh
sudo cp tools/99-pwrlog.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
```

## Host tools

`tools/requirements.txt`:

```sh
python3 -m venv ~/.venvs/pwrlog
~/.venvs/pwrlog/bin/pip install -r tools/requirements.txt
```

### `pwrctl.py` — device control + live BLE monitor

Auto-detects the port via VID:PID. CLI surface mirrors the on-device `pwr` shell:

```sh
pwrctl status                  # state, time, uptime
pwrctl log | stop | rotate     # control logging
pwrctl offload | resume        # MSC offload mode
pwrctl time set now            # sync wall clock
pwrctl config list             # NVS-backed settings
pwrctl monitor                 # live BLE telemetry (Ctrl-C to quit)
```

### `pwrdl.py` — TDMS download over vendor bulk

Bypasses USB MSC mount/unmount. CDC for the command channel + vendor bulk for the body:

```sh
pwrdl.py /dev/ttyACM11 --list
pwrdl.py /dev/ttyACM11 log0009.tdms session.tdms     # ~560 KB/s, CRC-verified
pwrdl.py /dev/ttyACM11 --record 10                   # record 10 s, then download newest
```

Body framing: `b'PWR\x01' + uint32-LE size + body + uint32-LE crc32-IEEE`. The host verifies the CRC; a mismatch aborts.

### `pwranalyze.py` — TDMS inspector

```sh
pwranalyze.py info session.tdms          # file metadata, channels, sample rates
pwranalyze.py stats session.tdms         # per-channel min/mean/max/RMS
pwranalyze.py plot session.tdms          # quick-look matplotlib plot
pwranalyze.py fft session.tdms --chan vbus
```

## Configuration: NVS + SD-side `pwr.cfg`

Two parallel paths to the same `struct app_cfg`:
- **NVS** (live), edited via `pwr config set key value` from the shell or BLE.
- **`/SD:/pwr.cfg`** (offline), INI-style key=value file. `pwr config import` applies it; `pwr config export` writes the live config back. `cfgfile.load_on_boot=true` auto-applies on every mount.

Keys are documented in `src/config.c` (the `cfg_items[]` table is authoritative).

## Debug build

The production build has logging stripped to reclaim RAM (`CONFIG_LOG=n`, no RTT, no thread analyzer). When you need crash dumps, boot diagnostics, or thread stack high-water reports, rebuild with the debug overlay:

```sh
west build ... --sysbuild -- -DCONFIG_APP_BRINGUP=n -DEXTRA_CONF_FILE=prj-debug.conf
```

`prj-debug.conf` re-enables `CONFIG_LOG=y`, RTT, the log subsystem buffers, and the thread analyzer. Logs available on `pyocd rtt -t nrf52840 -M attach -a 0x20000000 -s 0x40000` and on the CDC console.

## Layout

```
prj.conf                            production build config (LOG=n, RTT off)
prj-debug.conf                      overlay: re-enable LOG + RTT + analyzer
sysbuild.conf                       MCUboot + signed-app composition
boards/promicro_nrf52840_nrf52840.overlay   peripheral pin map (final)

src/
  main.c            init + mode state machine (logging ↔ offload ↔ sleep)
  board_init.c      board_early_init_hook(): UICR.REGOUT0 = 3.3 V
  app_config.h      tunables + front-end scaling constants
  timebase.[ch]     1 MHz master clock, 32→64-bit tick extension
  acquisition.[ch]  nrfx SAADC + TIMER + PPI, 20 kSPS gapless DMA
  ina228.[ch]       I²C precision channel, CNVR hardware timestamps
  aggregate.[ch]    windowed RMS/avg/min/max for the BLE view
  stats.[ch]        rolling stats + HOLD latches + energy accumulator
  tdms.[ch]         NI TDMS segment encoder
  storage.[ch]      FatFs/exFAT mount/unmount + storage_read_file
  logger.[ch]       SD-writer thread; ties acquisition → TDMS → SD
  logctl.[ch]       logging state machine (start/stop/rotate)
  ble_stream.[ch]   BLE GATT: telemetry NOTIFY + control characteristic
  usb_msc.[ch]      composite USB: CDC + MSC + vendor-bulk
  usbd_pwr_bulk.[ch] vendor-class bulk-IN endpoint, 8 in-flight transfers
  display.[ch]      SSD1306 lifecycle + render thread
  ui.[ch]           page state + per-page renderers
  icons_font.c      16×16 status-bar icons (Open Iconic + hand-rolled batteries)
  controls.[ch]     buttons, debouncing, LED patterns
  cli.[ch]          Zephyr shell commands (`pwr ...`)
  config.[ch]       struct app_cfg + NVS settings + type system
  cfgfile.[ch]      INI parser for /SD:/pwr.cfg
  clock.[ch]        wall-clock estimator on top of timebase + RTC
  power.[ch]        battery sense, sleep entry, fuel-gauge curve
  cal.[ch]          gain/offset calibration (fast + slow channels)
  bringup.[ch]      CONFIG_APP_BRINGUP=y self-test image (alternative main)

tools/
  pwrctl.py         shell + BLE control / monitor
  pwrdl.py          high-throughput TDMS download (vendor bulk)
  pwranalyze.py     TDMS inspector / stats / plot / FFT
  99-pwrlog.rules   udev rule for non-root vendor-class access
  requirements.txt  pip dependencies
```

## Deeper docs

- `REQUIREMENTS.md` — channel specs, accuracy targets, deliverables.
- `IMPLEMENTATION.md` — architectural decisions, locking model, timing.
- `DATA_FORMAT.md` — TDMS schema (groups, properties, channels).
- `VERIFY_RESULTS.md` — verification campaign log + measured numbers.

## Provenance

Fully open source. Builds against upstream Zephyr 4.4 + hal_nordic/nrfx (open) + Zephyr's SW BLE controller (open). No Nordic SDK, no SoftDevice, no proprietary BLE stack.
