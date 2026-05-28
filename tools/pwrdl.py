#!/usr/bin/env python3
"""
pwrdl.py -- download a TDMS session from a pwr-logger over USB CDC.

Workflow that doesn't require the MSC mount/unmount dance:

    $ ./pwrdl.py /dev/ttyACM7 --list
    20260527T203743Z.tdms     15115264
    log0030.tdms                160645
    (2 files, 15 275 909 bytes)

    $ ./pwrdl.py /dev/ttyACM7 20260527T203743Z.tdms session.tdms
    saved 15115264 bytes -> session.tdms (CRC ok)

Frame format on the wire (sent by the device's `pwr download <file>`
command):

    header = b'PWR\\x01' + <uint32 LE size>
    body   = <size> raw bytes
    footer = <uint32 LE crc32-IEEE>

Any text the shell emits before the magic (prompt, command echo) is
harmlessly skipped while scanning for `PWR\\x01`. CRC is the standard
zlib CRC-32 so Python's stdlib can verify directly.

Usage examples:

    # list sessions on the SD
    pwrdl.py /dev/ttyACM7 --list

    # download one file
    pwrdl.py /dev/ttyACM7 <name> [<dest>]

    # one-shot test loop: record N seconds, then pull the latest file
    pwrdl.py /dev/ttyACM7 --record 10 out.tdms
"""

import argparse
import re
import struct
import sys
import time
import zlib

import serial   # pip install pyserial

MAGIC = b'PWR\x01'


def send_cmd(s, cmd, settle=0.4):
    """Write a shell command + CR, give the device a moment to act."""
    s.reset_input_buffer()
    s.write(cmd.encode() + b'\r\n')
    s.flush()
    time.sleep(settle)


def read_until_magic(s, deadline):
    """Consume bytes from `s` until MAGIC appears. Returns the bytes that
    arrived AFTER the magic (i.e. the start of the size header), so the
    caller can prepend them to the next read. We read in chunks rather
    than 1 byte at a time so the tty buffer doesn't overflow while we
    scan."""
    buf = bytearray()
    while time.monotonic() < deadline:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
            idx = buf.find(MAGIC)
            if idx >= 0:
                return bytes(buf[idx + len(MAGIC):])
    raise TimeoutError("timeout waiting for download magic (PWR\\x01)")


def read_exact(s, n, deadline):
    """Read exactly n bytes or raise TimeoutError."""
    buf = bytearray()
    while len(buf) < n:
        if time.monotonic() > deadline:
            raise TimeoutError(f"read_exact: got {len(buf)}/{n} bytes")
        chunk = s.read(min(4096, n - len(buf)))
        if chunk:
            buf += chunk
    return bytes(buf)


def download(s, name, dest, timeout=60.0):
    """Send `pwr download <name>` and start reading immediately. Do NOT
    settle-sleep after the write: the device starts streaming at ~85 KB/s
    within a millisecond of receiving the command's newline, and Linux's
    tty input buffer is small (~4 KB default), so any pause before we
    start reading drops bytes."""
    deadline = time.monotonic() + timeout
    s.reset_input_buffer()
    s.write(f"pwr download {name}\r\n".encode())
    s.flush()
    # No sleep -- straight into the read loop.
    after_magic = read_until_magic(s, deadline)
    # The post-magic bytes already include the start of our payload;
    # consume them first before pulling more from the wire.
    def take_n(n):
        nonlocal after_magic
        while len(after_magic) < n:
            chunk = s.read(min(4096, n - len(after_magic)))
            if chunk:
                after_magic += chunk
            elif time.monotonic() > deadline + 5:
                raise TimeoutError(f"take_n short: {len(after_magic)}/{n}")
        out = bytes(after_magic[:n])
        after_magic = after_magic[n:]
        return out

    size = struct.unpack('<I', take_n(4))[0]
    deadline = max(deadline, time.monotonic() + size / 30_000)
    body = take_n(size)
    crc_expected = struct.unpack('<I', take_n(4))[0]
    crc_actual = zlib.crc32(body) & 0xffffffff
    if crc_actual != crc_expected:
        raise ValueError(
            f"CRC mismatch: got {crc_actual:08x} expected {crc_expected:08x}"
        )
    with open(dest, 'wb') as f:
        f.write(body)
    return size


def ls(s, timeout=5.0):
    """Run `pwr ls` and parse the lines. Returns [(name, size), ...]."""
    send_cmd(s, "pwr ls")
    deadline = time.monotonic() + timeout
    raw = bytearray()
    while time.monotonic() < deadline:
        chunk = s.read(1024)
        if chunk:
            raw += chunk
        # Trailer marker that closes the listing.
        if b'files,' in raw:
            break
    text = raw.decode('utf-8', errors='replace')
    out = []
    for line in text.splitlines():
        m = re.match(r"^(\S+\.tdms)\s+(\d+)\s*$", line)
        if m:
            out.append((m.group(1), int(m.group(2))))
    return out


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
        # newest by name (UTC-stamped names sort chronologically by design)
        files.sort(key=lambda x: x[0])
        args.name = files[-1][0]

    if not args.name:
        p.error("need a <name> (or use --list / --record)")

    dest = args.dest or args.name
    size = download(s, args.name, dest, timeout=args.timeout)
    print(f"saved {size} bytes -> {dest} (CRC ok)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
