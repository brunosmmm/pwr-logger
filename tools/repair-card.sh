#!/bin/sh
# Repair the pwr-logger SD card's exFAT filesystem via the device's MSC drive.
# Usage: put the logger in `pwr offload`, then:  sudo tools/repair-card.sh /dev/sdX1
# Afterwards run `pwr resume`. Never run this while the logger is recording.
set -eu
dev="${1:?usage: $0 /dev/sdX1}"
[ "$(id -u)" -eq 0 ] || { echo "run with sudo" >&2; exit 1; }
[ "$(lsblk -no LABEL "$dev")" = "PWRLOG" ] || { echo "$dev is not the PWRLOG volume; refusing" >&2; exit 1; }
if findmnt -S "$dev" >/dev/null; then umount "$dev"; fi
exec fsck.exfat -r "$dev"
