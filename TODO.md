# TODO

Tracked items that are real, scoped work but not yet started. Move
items out of here as commits land; delete when done.

## BLE: ATT MTU regression — rec_slow dropped silently

**Symptom:** `pwrctl monitor` shows `current/voltage/power/temp (INA228)` as
"—" while fast-channel + battery + state render normally. Started showing up
around commit `adca56c` (mcumgr SMP transport added).

**Cause:** `rec_slow` is 21 bytes; default ATT MTU is 23 bytes so the
notification payload limit is 20 bytes. `bt_gatt_notify()` returns
`-EMSGSIZE`; `ble_stream_publish_slow`'s caller in `logger.c:128`
discards the return value so the failure is invisible from device side.
`rec_fast` (15 B) and `rec_status` (8 B) still fit and render fine.

**Fix options:**

1. Anchor the ATT MTU explicitly in `prj.conf`:
   ```kconfig
   CONFIG_BT_ATT_PREALLOC_BUFFERS=y
   CONFIG_BT_ATT_MAX_MTU=247
   ```
   Costs a bit of RAM for the pre-allocated buffers; cheapest.

2. Have the device initiate the MTU exchange in the `connected` callback
   via `bt_gatt_exchange_mtu()` so we don't depend on the central doing
   it before the first slow notify. More robust but ~15 lines of code.

Option (1) first; if MTU exchange still races on some centrals, layer (2)
on top.

## BLE offload: pull TDMS sessions over the air (untethered case)

**Use case:** quick capture when there's no USB host nearby. USB pwrdl
remains the right channel when tethered (~560 KB/s); BLE is for the
"battery + sensor only, where am I going to plug in" workflow.

**Cheapest path:** `mcumgr fs_mgmt`. We already have mcumgr + the SMP
transport. Enable:
```kconfig
CONFIG_MCUMGR_GRP_FS=y
CONFIG_MCUMGR_GRP_FS_FILE_ACCESS_HOOK=y
```
Host: `mcumgr fs download /SD:/logNNNN.tdms session.tdms`.

**Throughput expectations:**
- mcumgr fs_mgmt: ~10-25 KB/s -> 15 MB in 10-25 min
- Custom GATT notify stream: ~50-80 KB/s -> 15 MB in 4-5 min
- L2CAP CoC (LE 2M PHY + DLE): ~100-180 KB/s -> 15 MB in ~90 s

Start with fs_mgmt. Only move to L2CAP CoC if the use case demands the
3-10x speedup; the host side gets more complex (BlueZ + bleak's
`open_l2cap_channel` quirks).

## BLE OTA: faster uploads

Current `pwrctl ota` push runs at ~2 KB/s -- a 268 KB image takes
~2 minutes. Bottleneck is per-chunk BLE round-trip latency, not
bandwidth or CPU.

**Three levers, in order of effort:**

1. **Larger SMP payload per chunk.** Currently 112 B (conservative CBOR
   overhead math). Bumping toward ~180 B (closer to MTU-244 cap) gives
   ~1.6x fewer round-trips with no other change. Pure host-side edit
   in `tools/pwrctl.py`.

2. **Shorter BLE connection interval.** Default ~30 ms; request
   7.5-15 ms from the central via `bt_conn_le_param_update` on the
   device side when a DFU upload starts. Halves or quarters per-chunk
   round-trip time. Limited by what the central accepts (BlueZ
   usually obliges peripheral preference).

3. **Send-window pipelining.** mcumgr SMP supports a send-ahead
   window so the host can have N chunks in flight before getting acks.
   Biggest win (potentially 3-5x) but adds real complexity to the
   pwrctl tool. Some external SMP clients (mcumgr CLI, newer
   smpclient) implement this.

Combined estimate: 4-6x speedup -> 25-40 s for a full image instead of
~140 s. Not blocking anything; do when BLE OTA becomes a daily-use
path rather than an occasional one.

## PM_DEVICE bus suspend: validate it actually lowers sleep current

The LS_IDLE rail-off + i2c0/i2c1/spi2 PM_DEVICE_ACTION_SUSPEND + CNVR
pull-up release was supposed to plug the mA-level leakage path
(MCU driving lines into unpowered slaves' ESD diodes -> ground).
The work landed in commit f44d9c1; running firmware reports being in
LS_IDLE; current draw still feels too high.

**Verify with a bench supply** (the user's calibration rig is the
right tool): put device in LS_IDLE with USB disconnected, measure
battery rail current. Should be sub-mA, ideally <100 uA if the bus
suspend is real.

If draw is still mA-level, look for:
- pinctrl-sleep state not actually applied -- the bus drivers may not
  honor PM_DEVICE_ACTION_SUSPEND on this Zephyr/nRF version. Confirm
  with a scope on SDA/SCL/SCK after rail drops.
- Other always-driven GPIOs: status LED (P0.9), button pull-ups
  (REC/ROTATE/OFFLOAD all have GPIO_PULL_UP -- closed switch on
  battery rail = 230 uA per).
- The board's own quiescent path (power LED, charger IC, divider).

If pinctrl-sleep isn't being applied, the manual workaround is to
explicitly nrf_gpio_cfg_default each switched-rail pin before
power_rail(false) and reconfigure on wake.

## Architectural: single OTA controller

Partially addressed in commit (this one): both BLE-mcumgr and SD-card
OTA paths now call ota_enter_safe_state() / ota_exit_safe_state() so
they share the "stop logger + set flag + show banner" prep.

Still divergent:
- BLE path: mcumgr owns the upload state machine. We only observe via
  DFU_STARTED/PENDING/STOPPED callbacks and react.
- SD path: ota_apply_from_sd is a single monolithic function that
  drives the whole pipeline directly.

A true unified controller would push BOTH into a single state machine
(e.g., in logctl or a new ota_ctrl module) that owns: "image source",
"upload progress", "post-transfer mark + reset", with the two
transports (BLE chunk, SD file) as plug-in implementations. Larger
refactor; do when the divergence causes a real bug rather than just
being architecturally ugly.

## Notes for both

- File-system access list: gate `fs_mgmt` to `/SD:/` only via the
  `CONFIG_MCUMGR_GRP_FS_FILE_ACCESS_HOOK` callback so a remote can't read
  NVS / flash partitions.
- Connection params: when starting a bulk transfer, request the shortest
  connection interval the central will accept (`bt_conn_le_param_update`)
  to maximize per-second packet count.
