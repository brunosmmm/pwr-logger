#!/usr/bin/env python3
"""
pwrdl_bulk.py -- high-throughput download via the pwr-logger's vendor-class
bulk endpoint. Uses pyserial (CDC) for shell commands and pyusb (libusb)
for the actual bulk read.

Same protocol as pwrdl.py:
    header = b'PWR\\x01' + <uint32 LE size>
    body   = <size> raw bytes
    footer = <uint32 LE crc32-IEEE>

The vendor interface (#3, bulk-IN endpoint 0x84) accepts multiple in-flight
USB transfers concurrently, bypassing the Zephyr CDC ACM single-transfer
ceiling (~160 KB/s). Expected throughput: 300-700 KB/s on USB FS bulk.

Usage:
    pwrdl_bulk.py /dev/ttyACMN <name> [<dest>]

Linux: needs libusb access permission. If you see 'No backend available'
or 'Access denied', install a udev rule:

    SUBSYSTEM=="usb", ATTRS{idVendor}=="2fe3", ATTRS{idProduct}=="0006", MODE="0666"

into /etc/udev/rules.d/99-pwrlog.rules and reload (`udevadm control --reload`).
"""

import argparse
import struct
import sys
import time
import zlib

import serial
import usb.core
import usb.util

VID = 0x2fe3
PID = 0x0006
VENDOR_IFACE = 3       # set by interface enumeration order
BULK_IN_EP = 0x84      # set by the descriptor; verify with lsusb if it shifts

MAGIC = b'PWR\x01'


def _claim(dev):
    """Claim our vendor interface. Detach kernel driver if attached
    (Linux often binds nothing to vendor-class interfaces, but be safe)."""
    cfg = dev.get_active_configuration()
    if dev.is_kernel_driver_active(VENDOR_IFACE):
        dev.detach_kernel_driver(VENDOR_IFACE)
    usb.util.claim_interface(dev, VENDOR_IFACE)


def _release(dev):
    try:
        usb.util.release_interface(dev, VENDOR_IFACE)
    except Exception:
        pass


def open_bulk():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        raise RuntimeError(f"pwr-logger {VID:04x}:{PID:04x} not found")
    _claim(dev)
    return dev


def open_shell(port, baud=115200):
    return serial.Serial(port, baud, timeout=0.5)


def read_until_magic(dev, deadline, transfer_size=4096, timeout_ms=200):
    """Read bulk-IN until MAGIC appears, return bytes that arrived
    AFTER the magic."""
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
    raise TimeoutError("timeout waiting for download magic (PWR\\x01)")


def read_exact(dev, n, deadline, prefix=b'', transfer_size=4096,
               timeout_ms=500):
    buf = bytearray(prefix)
    while len(buf) < n:
        if time.monotonic() > deadline:
            raise TimeoutError(f"read_exact: got {len(buf)}/{n}")
        try:
            chunk = dev.read(BULK_IN_EP, min(transfer_size, n - len(buf)),
                             timeout=timeout_ms)
        except usb.core.USBTimeoutError:
            continue
        if chunk:
            buf += bytes(chunk)
    return bytes(buf[:n]), bytes(buf[n:])


def download(shell, bulk, name, dest, timeout=60.0):
    """Send `pwr download <name>` over the shell CDC, read the framed
    binary over the vendor bulk endpoint."""
    deadline = time.monotonic() + timeout

    shell.reset_input_buffer()
    shell.write(f"pwr download {name}\r\n".encode())
    shell.flush()
    # No settle sleep -- device starts bulk-streaming within ms.

    leftover = read_until_magic(bulk, deadline)

    size_b, leftover = read_exact(bulk, 4, deadline, prefix=leftover)
    size = struct.unpack('<I', size_b)[0]

    deadline = max(deadline, time.monotonic() + size / 100_000)   # 100 KB/s floor

    body, leftover = read_exact(bulk, size, deadline, prefix=leftover)
    crc_b, _ = read_exact(bulk, 4, deadline + 5, prefix=leftover)
    crc_expected = struct.unpack('<I', crc_b)[0]
    crc_actual = zlib.crc32(body) & 0xffffffff
    if crc_actual != crc_expected:
        raise ValueError(
            f"CRC mismatch: got {crc_actual:08x} expected {crc_expected:08x}"
        )
    with open(dest, 'wb') as f:
        f.write(body)
    return size


def main():
    p = argparse.ArgumentParser()
    p.add_argument('port', help="shell CDC, e.g. /dev/ttyACM7")
    p.add_argument('name')
    p.add_argument('dest', nargs='?')
    p.add_argument('--baud', type=int, default=115200)
    p.add_argument('--timeout', type=float, default=60.0)
    args = p.parse_args()

    shell = open_shell(args.port, args.baud)
    bulk = open_bulk()
    try:
        dest = args.dest or args.name
        t0 = time.monotonic()
        size = download(shell, bulk, args.name, dest, timeout=args.timeout)
        t = time.monotonic() - t0
        print(f"saved {size} bytes -> {dest} (CRC ok)")
        print(f"elapsed: {t:.2f}s, throughput: {size/t/1024:.1f} KB/s")
        return 0
    finally:
        _release(bulk)
        shell.close()


if __name__ == '__main__':
    sys.exit(main())
