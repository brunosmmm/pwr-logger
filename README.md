# pwr-logger

Voltage / current / power logger on the **ProMicro nRF52840**, built with **upstream Zephyr** (fully open source — no proprietary blobs).

- **Fast current** channel: INA240 → nRF52840 SAADC @ 200 kSPS (DMA + PPI).
- **Precision** channel: INA228 over I²C (V / I / P / energy, hardware-timestamped).
- **Storage**: full-rate logging to SD as **NI TDMS**.
- **Live view**: aggregated stream over **BLE** (Zephyr's open SW controller — no blob).
- **Service**: OTA (mcumgr over BLE/USB, or a signed image from the SD card) and
  SD offload (USB-MSC), MCUboot flashed over SWD in place of the stock UF2 bootloader.

Design rationale lives in **`REQUIREMENTS.md`**, **`IMPLEMENTATION.md`**, and
**`DATA_FORMAT.md`** — read those first.

## Status

⚠️ **Scaffold.** The structure, build files, devicetree overlay, and module
decomposition are in place; the peripheral logic is stubbed (`TODO` markers point
to the relevant design-doc sections). It **builds clean** with sysbuild against
upstream **Zephyr 4.4** (MCUboot + signed app; FLASH ~7%, RAM ~12%), verified on
this machine — not yet exercised on hardware. Note: **mcumgr / BLE-OTA is
deferred** in `prj.conf` (needs extra deps); the rest is enabled (open BLE
controller, SD/exFAT, nrfx acquisition drivers).

⚠️ **Pin assignments in `boards/promicro_nrf52840_nrf52840.overlay` are
placeholders** — set them per the wiring plan before building for hardware.

## Prerequisites

Install the **Zephyr SDK** (GCC toolchain) + `west` per the Zephyr Getting
Started Guide. Then `west init -l . && west update` fetches upstream Zephyr and
its modules (hal_nordic/nrfx, MCUboot, FatFs). Pin `west.yml`'s `revision:` to a
release tag (e.g. v4.x). Fully open source — no Nordic SDK or blobs required.

## Build & flash

```sh
# one-time, if using this repo as a west workspace:
west init -l .
west update

# build (note: the plain target, NOT .../uf2 — we replace UF2 with MCUboot)
west build -b promicro_nrf52840/nrf52840 --sysbuild

# alternatively, build against an EXISTING Zephyr workspace (no separate init) —
# this is how it was verified (upstream Zephyr 4.4):
#   cd <zephyr-workspace>
#   west build -s <path-to-pwr-logger> -b promicro_nrf52840/nrf52840 \
#       -d <path-to-pwr-logger>/build --sysbuild

# first flash over SWD does a full erase, removing the stock UF2 bootloader:
west flash --runner jlink        # or pyocd / openocd for CMSIS-DAP
```

SWD is on small pads (often the underside) — solder fly-leads. A spare nRF52840
DK works as a J-Link via its Debug-Out header. After this, dev = SWD, OTA =
mcumgr over BLE/USB.

## Layout

```
prj.conf / sysbuild.conf   build + partition config
boards/promicro_nrf52840_nrf52840.overlay          peripheral pin map (placeholders)
src/
  main.c          init + mode state machine (logging ↔ offload)
  app_config.h    tunables + front-end scaling constants
  timebase.[ch]   1 MHz master clock, 32→64-bit tick extension
  acquisition.[ch] nrfx SAADC + TIMER + PPI, DMA buffer pool
  ina228.[ch]     I²C precision channel (CNVR hardware timestamp)
  aggregate.[ch]  windowed RMS/avg/min/max for the BLE view
  tdms.[ch]       NI TDMS segment encoder
  storage.[ch]    storage seam; exFAT/FatFs backend
  logger.[ch]     SD-writer thread; ties acquisition → TDMS → SD/BLE
  ble_stream.[ch] BLE live-view GATT service (+ OTA transport)
  usb_msc.[ch]    USB Mass Storage offload (mode-switched)
```

## First implementation milestones

1. `timebase` + `acquisition`: real nrfx SAADC+TIMER+PPI, verify 200 kSPS gapless
   double-buffering and the captured timestamps (scope a known input).
2. `tdms`: emit a valid file; confirm with an **npTDMS** round-trip early.
3. `storage`: `f_expand` contiguous files + file rolling.
4. `ina228`: configure/calibrate + CNVR-triggered timestamped reads.
5. `ble_stream`, `usb_msc`, OTA, and the mode state machine.

A host-side **npTDMS** reader script (validation + plotting) should land
alongside milestone 2.
