# Recovery images

Each entry here is a tested, OTA-able firmware binary kept outside `/tmp` so
it survives reboots and tmpfs sweeps. Use for emergency recovery: copy the
matching `.signed.bin` to `/SD:/firmware.bin` and `pwr ota apply` (or push via
BLE mcumgr-SMP).

For a probe-based recovery (chip erase, MCUboot reflash), use the `.mcuboot.hex`
plus the `.signed.hex`. `pyocd erase --chip` followed by flashing both.

## v1.0 — 2026-05-28

Tagged as `v1.0` (git tag). Verified safety net: revert tested
end-to-end (BAD-NOCONFIRM image swapped in, did not confirm, MCUboot
reverted on next reset). BLE-SMP transport enumeration confirmed:
service `8d53dc1d-1db7-4cd3-868b-8a527460aa84` exposed.

| File | md5 |
|---|---|
| pwrlog-v1.0.signed.bin   | 2701d9c902ddb4401bbd86929f07c372 |
| pwrlog-v1.0.signed.hex   | 4225b944b34f4617b2df294498c95521 |
| pwrlog-v1.0.mcuboot.hex  | 591fdfa151b0f0bd5c7fd3866bc9aff3 |

Build: `west build -s ~/work/pwr-logger -b promicro_nrf52840/nrf52840 --sysbuild -- -DCONFIG_APP_BRINGUP=n`
