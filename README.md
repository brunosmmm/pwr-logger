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

Verified as a freestanding app against an existing upstream Zephyr 4.4 workspace.
Probe: **Raspberry Pi Debug Probe (CMSIS-DAP) → OpenOCD** (the board's default
`jlink` runner does not apply; `pyocd` has issues here — see memory notes).

### Bring-up firmware (standalone, no bootloader) — boots today
```sh
cd <zephyr-workspace>
west build -s <pwr-logger> -b promicro_nrf52840/nrf52840 -d /tmp/pwrlog-bringup \
    -p always -- -DEXTRA_CONF_FILE=bringup.conf            # links the app at 0x0

openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg -c "adapter speed 4000" \
  -c init -c halt -c "nrf5 mass_erase" \
  -c "flash write_image erase /tmp/pwrlog-bringup/zephyr/zephyr.hex" \
  -c "verify_image /tmp/pwrlog-bringup/zephyr/zephyr.hex" \
  -c "reset run" -c shutdown
```
Logs: `pyocd rtt -t nrf52840` (interactive terminal) or the USB-CDC console at
`/dev/ttyACM*` (VID `2fe3`). The P0.15 LED blinks every 2 s.

### Full firmware (MCUboot + signed app) — WIP
`west build -b promicro_nrf52840/nrf52840 --sysbuild` builds, but the MCUboot
image currently mis-links to 0xc000 instead of 0x0 (see IMPLEMENTATION "Build
status"); being fixed before OTA works.

SWD is on small pads (often the underside) — solder fly-leads.

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
