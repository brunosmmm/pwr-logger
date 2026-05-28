#!/usr/bin/env python3
"""
pwrdl.py -- download a TDMS session from a pwr-logger over USB.

The tool drives the device with two USB interfaces:

  * SHELL CDC (pyserial) — for `pwr ls`, `pwr stop/log`, control commands.
  * VENDOR BULK (pyusb / libusb) — for the actual file body during
    `pwr download`. The pwr-logger advertises a vendor-specific interface
    (class 0xFF, bulk-IN endpoint 0x84) with multiple in-flight transfers,
    sustaining ~325 KB/s vs the ~85 KB/s the CDC ceiling allows. The
    device-side download path ships the body over the vendor endpoint
    only — there is no CDC fallback.

The bulk path needs pyusb installed AND a Linux udev rule so libusb can
claim the vendor interface. tools/99-pwrlog.rules has the rule template;
install once:

    sudo cp tools/99-pwrlog.rules /etc/udev/rules.d/
    sudo udevadm control --reload && sudo udevadm trigger

(Or run pwrdl.py itself with sudo for one-off testing.)

Workflow that doesn't require the MSC mount/unmount dance:

    $ ./pwrdl.py /dev/ttyACM7 --list
    20260527T203743Z.tdms     15115264
    log0030.tdms                160645
    (2 files, 15 275 909 bytes)

    $ ./pwrdl.py /dev/ttyACM7 20260527T203743Z.tdms session.tdms
    saved 15115264 bytes -> session.tdms (CRC ok, 344 KB/s)

Frame format on the bulk endpoint:

    header = b'PWR\\x01' + <uint32 LE size>
    body   = <size> raw bytes
    footer = <uint32 LE crc32-IEEE>
"""

import argparse
import re
import struct
import sys
import time
import zlib

import serial   # pip install pyserial

try:
    import usb.core
    import usb.util
except ImportError as e:
    sys.stderr.write(
        "pwrdl: pyusb is required (pip install pyusb). " + str(e) + "\n"
    )
    sys.exit(2)


MAGIC = b'PWR\x01'

VID = 0x2fe3
PID = 0x0006
VENDOR_IFACE = 3      # interface number for the vendor-class bulk interface
BULK_IN_EP   = 0x84   # endpoint address inside that interface


# ---- CDC (shell) helpers ----------------------------------------------------

PROMPT = b'uart:~$'


def wait_for_prompt(s, timeout=3.0):
    """Drain bytes from `s` until the Zephyr shell prompt appears.

    Successive `pwr download` calls can otherwise race: pwrdl.py exits,
    re-launches, sends the next command while the device's prior
    cmd_download is still in cleanup (fs_close + storage_unmount).
    The shell queues the new command, the host opens the bulk endpoint
    and times out waiting for magic that never arrives. Waiting for a
    prompt before issuing the command serializes us cleanly."""
    deadline = time.monotonic() + timeout
    s.write(b'\r\n')          # nudge a fresh prompt
    s.flush()
    buf = bytearray()
    while time.monotonic() < deadline:
        chunk = s.read(256)
        if chunk:
            buf += chunk
            if PROMPT in buf:
                # Drain a moment so the prompt is fully consumed before
                # the next command lands on top of it.
                time.sleep(0.05)
                s.reset_input_buffer()
                return True
    return False


def send_cmd(s, cmd, settle=0.4):
    """Write a shell command + CR, give the device a moment to act."""
    s.reset_input_buffer()
    s.write(cmd.encode() + b'\r\n')
    s.flush()
    time.sleep(settle)


def ls(s, timeout=5.0):
    """Run `pwr ls` and parse the lines. Returns [(name, size), ...]."""
    send_cmd(s, "pwr ls")
    deadline = time.monotonic() + timeout
    raw = bytearray()
    while time.monotonic() < deadline:
        chunk = s.read(1024)
        if chunk:
            raw += chunk
        if b'files,' in raw:
            break
    text = raw.decode('utf-8', errors='replace')
    out = []
    for line in text.splitlines():
        m = re.match(r"^(\S+\.tdms)\s+(\d+)\s*$", line)
        if m:
            out.append((m.group(1), int(m.group(2))))
    return out


# ---- transport: vendor bulk -------------------------------------------------

def open_bulk():
    """Open + claim the vendor bulk interface. Exits with a clear error if
    the device isn't present or the kernel won't let us touch the interface
    (typically: missing udev rule on Linux). The device-side download path
    sends the body via the vendor endpoint only, so there's no useful
    fallback to silently take."""
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        sys.stderr.write(
            f"pwrdl: pwr-logger USB device {VID:04x}:{PID:04x} not found "
            "(is it plugged in and enumerated?)\n"
        )
        sys.exit(3)
    try:
        if dev.is_kernel_driver_active(VENDOR_IFACE):
            dev.detach_kernel_driver(VENDOR_IFACE)
        usb.util.claim_interface(dev, VENDOR_IFACE)
        return dev
    except usb.core.USBError as e:
        sys.stderr.write(
            f"pwrdl: can't claim vendor interface (libusb: {e}).\n"
            "Linux: install tools/99-pwrlog.rules and reload udev:\n"
            "  sudo cp tools/99-pwrlog.rules /etc/udev/rules.d/\n"
            "  sudo udevadm control --reload && sudo udevadm trigger\n"
            "Or run this script with sudo for one-off testing.\n"
        )
        sys.exit(4)


def release_bulk(dev):
    if dev is None:
        return
    try:
        usb.util.release_interface(dev, VENDOR_IFACE)
    except Exception:
        pass


def _bulk_read(dev, n, deadline, prefix=b'',
               transfer_size=4096, timeout_ms=500):
    buf = bytearray(prefix)
    while len(buf) < n:
        if time.monotonic() > deadline:
            raise TimeoutError(f"bulk read short: {len(buf)}/{n}")
        try:
            chunk = dev.read(BULK_IN_EP,
                             min(transfer_size, n - len(buf)),
                             timeout=timeout_ms)
        except usb.core.USBTimeoutError:
            continue
        if chunk:
            buf += bytes(chunk)
    return bytes(buf[:n]), bytes(buf[n:])


def _bulk_read_until_magic(dev, deadline,
                           transfer_size=4096, timeout_ms=200):
    buf = bytearray()
    while time.monotonic() < deadline:
        try:
            chunk = dev.read(BULK_IN_EP, transfer_size, timeout=timeout_ms)
        except usb.core.USBTimeoutError:
            continue
        if chunk:
            buf += bytes(chunk)
            idx = buf.find(MAGIC)
            if idx >= 0:
                return bytes(buf[idx + len(MAGIC):])
    raise TimeoutError("timeout waiting for download magic on bulk EP")


# ---- unified download -------------------------------------------------------

def download(shell, name, dest, timeout=60.0):
    """Send `pwr download <name>` via the shell CDC, read the framed binary
    over the vendor bulk endpoint. Returns (size, throughput_kbps)."""
    bulk = open_bulk()
    deadline = time.monotonic() + timeout

    # Serialize against the prior command's tail (file close + unmount
    # take ~200-500 ms after a multi-MB download). Without this,
    # back-to-back invocations race and the second one's bulk read
    # times out waiting for magic.
    wait_for_prompt(shell)
    shell.write(f"pwr download {name}\r\n".encode())
    shell.flush()
    t0 = time.monotonic()

    try:
        after = _bulk_read_until_magic(bulk, deadline)
        size_b, after = _bulk_read(bulk, 4, deadline, prefix=after)
        size = struct.unpack('<I', size_b)[0]
        deadline = max(deadline, time.monotonic() + size / 100_000)
        body, after = _bulk_read(bulk, size, deadline, prefix=after)
        crc_b, _ = _bulk_read(bulk, 4, deadline + 5, prefix=after)
    finally:
        release_bulk(bulk)

    elapsed = time.monotonic() - t0
    crc_expected = struct.unpack('<I', crc_b)[0]
    crc_actual = zlib.crc32(body) & 0xffffffff
    if crc_actual != crc_expected:
        raise ValueError(
            f"CRC mismatch: got {crc_actual:08x} expected {crc_expected:08x}"
        )
    with open(dest, 'wb') as f:
        f.write(body)
    return size, (size / elapsed / 1024 if elapsed > 0 else 0.0)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('port', help="serial device, e.g. /dev/ttyACM7")
    p.add_argument('name', nargs='?',
                   help="filename on the SD (run --list to see them)")
    p.add_argument('dest', nargs='?',
                   help="local output path (default: same as <name>)")
    p.add_argument('--list', action='store_true',
                   help="list SD files instead of downloading")
    p.add_argument('--record', type=float, metavar='SEC',
                   help="run a record-for-N-seconds cycle then download the "
                        "newest file (overrides <name> if given)")
    p.add_argument('--baud', type=int, default=115200)
    p.add_argument('--timeout', type=float, default=120.0)
    args = p.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.5)

    if args.list:
        for name, sz in ls(s):
            print(f"{name:<32s} {sz:>10d}")
        return 0

    if args.record is not None:
        send_cmd(s, "pwr stop", settle=2.0)
        send_cmd(s, "pwr log",  settle=1.0)
        time.sleep(args.record)
        send_cmd(s, "pwr stop", settle=2.0)
        files = ls(s)
        if not files:
            print("no files after record", file=sys.stderr)
            return 1
        files.sort(key=lambda x: x[0])
        args.name = files[-1][0]

    if not args.name:
        p.error("need a <name> (or use --list / --record)")

    dest = args.dest or args.name
    size, rate = download(s, args.name, dest, timeout=args.timeout)
    print(f"saved {size} bytes -> {dest} (CRC ok, {rate:.0f} KB/s)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
