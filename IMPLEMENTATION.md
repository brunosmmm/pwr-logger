# pwr-logger — Implementation Plan

Decisions and structure for the firmware. See `REQUIREMENTS.md` for the *what*;
this is the *how*. Single-channel first build.

## Stack

- **Upstream Zephyr RTOS**, C — fully open source, **no proprietary blobs**.
  Deliberately upstream Zephyr (not the nRF Connect SDK) to keep the whole stack
  FOSS. BLE uses **Zephyr's software link-layer controller**
  (`CONFIG_BT_LL_SW_SPLIT`), not Nordic's closed SoftDevice Controller.
- **Hot path uses nrfx directly**: `nrfx_saadc` + `nrfx_timer` + `nrfx_ppi` for
  gapless, jitter-free 200 kSPS double-buffering. (PPI, not DPPI — this is the
  '840.) nrfx is open (BSD-3, via hal_nordic). Zephyr owns everything else —
  BLE host/controller, USB-MSC, FatFs/exFAT, MCUboot, mcumgr.
- Build: **`west` + sysbuild** (multi-image: app + MCUboot). Toolchain: the
  open-source **Zephyr SDK** (GCC).

## Target board

- **ProMicro nRF52840** — **upstream Zephyr board** `boards/others/promicro_nrf52840`.
  No custom board needed. **No external QSPI flash** (confirmed in the DTS),
  native USB.
- **Build target `promicro_nrf52840/nrf52840`** (the plain variant) — *not* the
  `…/nrf52840/uf2` variant. The `/uf2` target includes
  `nrf52840_partition_uf2_sdv6.dtsi` to coexist with the stock Adafruit UF2
  bootloader + SoftDevice v6; we deliberately bypass it.
- Stock **UF2 bootloader replaced with MCUboot**, flashed via **SWD** (full
  chip-erase + sysbuild merged image, MCUboot at 0x0). BLE uses Zephyr's open
  software controller — no proprietary radio blob.
- Board pre-enables `i2c0`, `i2c1`, `spi2` with pinctrl. We add a **project-level
  overlay** (`boards/promicro_nrf52840_nrf52840.overlay`) for our peripherals.
  Note `spi2` = SPIM2 (≤ 8 MHz ≈ 1 MB/s, still 2.5× the 400 kB/s need); define
  **`spi3` (SPIM3, 32 MHz)** in the overlay for SD if more headroom is wanted.
- SWD on small pads (often underside) → solder fly-leads. Probe: J-Link
  (`--runner jlink`), CMSIS-DAP (`pyocd`/`openocd`), or a spare nRF52840 DK as a
  J-Link via its Debug-Out header.
- Build/flash: `west build -b promicro_nrf52840/nrf52840 --sysbuild`,
  then `west flash` over SWD (full erase on first flash to remove UF2).

## Project layout

```
pwr-logger/
  west.yml                    # west manifest (upstream Zephyr as dependency)
  CMakeLists.txt
  prj.conf                    # Kconfig
  sysbuild.conf               # SB_CONFIG_BOOTLOADER_MCUBOOT=y
  boards/promicro_nrf52840_nrf52840.overlay   # SPI-SD, I2C-INA228, SAADC AIN, CNVR GPIO
  src/
    main.c                    # init + mode state machine (logging ↔ offload)
    app_config.h              # tunables + front-end scaling constants
    timebase.[ch]             # 1 MHz master clock; 32->64-bit tick extension
    acquisition.[ch]          # nrfx SAADC+timer+ppi, DMA buffer pool
    ina228.[ch]               # I2C precision channel (paced by CNVR pin)
    aggregate.[ch]            # decimation + RMS/avg/min/max windows
    tdms.[ch]                 # TDMS segment encoder (above storage seam)
    storage.[ch]              # storage seam; exFAT/FatFs backend (default)
    logger.[ch]               # SD-writer thread; acquisition -> TDMS -> SD/BLE
    ble_stream.[ch]           # GATT service: aggregates + status + control
    usb_msc.[ch]              # MSC class + FS hand-off for offload mode
```

## Key Kconfig (prj.conf highlights)

```
CONFIG_FPU=y
# acquisition (nrfx)
CONFIG_NRFX_SAADC=y
CONFIG_NRFX_TIMER2=y
CONFIG_NRFX_PPI=y
# precision channel
CONFIG_I2C=y
# storage
CONFIG_SPI=y
CONFIG_DISK_DRIVER_SDMMC=y
CONFIG_FILE_SYSTEM=y
CONFIG_FAT_FILESYSTEM_ELM=y
CONFIG_FS_FATFS_EXFAT=y
CONFIG_FS_FATFS_LFN=y
# BLE throughput
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_CTLR_PHY_2M=y
CONFIG_BT_CTLR_DATA_LENGTH_MAX=251
CONFIG_BT_L2CAP_TX_MTU=247
CONFIG_BT_BUF_ACL_TX_SIZE=251
# USB
CONFIG_USB_DEVICE_STACK=y
CONFIG_USB_MASS_STORAGE=y
# OTA (MCUboot enabled via sysbuild) + SMP
CONFIG_MCUMGR=y
CONFIG_MCUMGR_TRANSPORT_BT=y
CONFIG_MCUMGR_GRP_IMG=y
CONFIG_MCUMGR_GRP_OS=y
# external flash (only if the board has QSPI)
CONFIG_NORDIC_QSPI_NOR=y
```

## Acquisition design (nrfx)

- `nrfx_saadc`, 1 channel, 12-bit, `NRF_SAADC_ACQTIME_3US`. Reference/gain map the
  INA240's 0–3.3 V output: internal 0.6 V ref + gain 1/6 → 3.6 V full-scale
  (~0.88 mV/LSB), using ~92% of range.
- `nrfx_timer` @ 200 kHz; **PPI** wires `TIMER.COMPARE → SAADC.SAMPLE` (no CPU).
- Gapless double-buffer via nrfx events: supply next buffer on
  `NRFX_SAADC_EVT_BUF_REQ`, hand filled buffer to a `k_msgq` on `..._EVT_DONE`.
- **Buffer pool ≈ 8 × 16 KB = 128 KB** (~320 ms cushion at 400 kB/s). Whole
  buffers move by pointer — no byte-ring copy. SD-writer thread drains the msgq.
- Thread priorities: BLE (Zephyr) > SD-writer > aggregator. Acquisition lives in
  the nrfx ISR (minimal: post + re-arm).

## OTA / partitions

- MCUboot via **sysbuild** (`SB_CONFIG_BOOTLOADER_MCUBOOT=y`). Partitions come
  from **devicetree fixed-partitions** — the plain board target pulls in
  `<nordic/nrf52840_partition.dtsi>`, the standard nRF52840 MCUboot layout (no
  NCS Partition Manager / pm_static):
  ```
  boot_partition     0x00000  0x0c000   # 48 KB MCUboot
  slot0_partition    0x0c000  0x5b000   # app, 372 KB
  slot1_partition    0x67000  0x5b000   # secondary, 372 KB
  scratch_partition  0xc2000  0x36000   # 216 KB
  storage_partition  0xf8000  0x08000   # 32 KB settings/NVS
  ```
- App budget ~372 KB/slot. BLE (open controller) + USB-MSC + FatFs + mcumgr may
  approach it. If it overflows: override the layout in a DT overlay to **drop
  scratch** (swap-using-move needs none) and grow both slots to ~480 KB; trim
  features only as a last resort.
- DFU transports: BLE SMP (`mcumgr` / Device Manager app) + USB.
- Recovery: SWD reflash; MCUboot swap+revert covers bad OTA images.

## Bring-up / debug

- **SWD probe required** (no onboard debugger): J-Link / CMSIS-DAP / nRF52840 DK
  as J-Link. Needed anyway to install MCUboot over the stock UF2.
- First flash: `west flash` of the sysbuild merged image (full erase). Thereafter
  SWD for dev, mcumgr (BLE/USB) for OTA.
- Logging/trace: **RTT** (`CONFIG_USE_SEGGER_RTT`) over the same SWD link.

## Open / pending

- Pin assignments in the project overlay (`boards/promicro_nrf52840_nrf52840.overlay`):
  SD on SPI (spi2 default, or add spi3 for 32 MHz), INA228 on I2C, SAADC AIN for
  the INA240 output, GPIO for INA228 CNVR / control.
- Which SWD probe (sets `--runner`).
- Data record format + fast/slow timestamping (next design topic).
