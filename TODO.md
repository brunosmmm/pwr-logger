# TODO

Tracked items that are real, scoped work but not yet started. Move
items out of here as commits land; delete when done.

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

Pipeline runs at ~2 KB/s baseline (per-chunk round-trip bound). Three
levers; the first one has landed:

1. **DONE** (commit b60a08f): Larger SMP payload per chunk. Went from
   112 B to 182 B; expected ~1.6x speedup, dropping a 268 KB push from
   ~140 s to ~85 s. Pure host-side change.

2. **OPEN**: Shorter BLE connection interval **during OTA only**.
   Device already requests 15-30 ms on connect (faster than the ~67 ms
   default). Pushing to 7.5-15 ms via `bt_conn_le_param_update` when
   DFU_STARTED fires, restoring afterward, would give roughly another
   2x without permanently raising idle-mode BLE power. Needs the
   restore-on-DFU_STOPPED counterpart so it's not a one-way ratchet.

3. **OPEN**: Send-window pipelining. mcumgr SMP supports N chunks
   in flight before requiring acks. Biggest single lever (3-5x).
   Real complexity in `pwrctl.py` -- tracking acks vs the current
   sequence, handling out-of-order responses, retrying on drop.

(2) + (3) combined with (1) would put us in the 25-40 s range for a
full image. Worth doing if BLE OTA becomes a daily-use path.

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
