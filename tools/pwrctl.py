#!/usr/bin/env python3
"""pwrctl - host CLI for the pwr-logger.

Control over the USB serial console (Zephyr `pwr` shell) and a live BLE telemetry
view. BLE *control* arrives with the device's control characteristic (Step 6);
until then control commands go over serial and `monitor` uses BLE notifications.

Usage:
  pwrctl status
  pwrctl log | stop | rotate | offload | resume | poweroff
  pwrctl time get | time set [now|<unix-epoch>]
  pwrctl config list | config get <key> | config set <key> <value>
  pwrctl monitor                 # live BLE telemetry (Ctrl-C to quit)

Options:
  --port <dev>   serial port (default: auto-detect USB VID:PID 2fe3:0006)

Deps: rich, pyserial, bleak   (pip install rich pyserial bleak)
"""
import argparse
import re
import sys
import time
from enum import IntEnum

from rich.console import Console
from rich.panel import Panel
from rich.table import Table

console = Console()

VID, PID = 0x2FE3, 0x0006
SHELL_PROMPT = "uart:~$"
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

# fast-channel code -> amps (matches app_config / firmware)
SLOPE = 3.6 / (2 ** 14 * 200 * 0.001)      # 0.0010986 A/code
INTER = -1.6622 / (200 * 0.001)            # -8.311 A (calibrated VREF)
TELE_UUID = "f0a00001-1234-5678-9abc-def012345678"
CTRL_UUID = "f0a00002-1234-5678-9abc-def012345678"


class Op(IntEnum):
    """Control opcodes written to the control characteristic (match ble_stream.c).
    Member names are the CLI command names, upper-cased."""
    LOG = 0x01
    STOP = 0x02
    ROTATE = 0x03
    OFFLOAD = 0x04
    RESUME = 0x05
    POWEROFF = 0x06
    KEEPALIVE = 0x07
    SETTIME = 0x10


class State(IntEnum):
    """Device mode (matches enum logctl_state)."""
    STOPPED = 0
    LOGGING = 1
    OFFLOAD = 2
    SHIP = 3

    @property
    def color(self):
        return {State.LOGGING: "green", State.STOPPED: "yellow",
                State.OFFLOAD: "cyan", State.SHIP: "red"}[self]

    @classmethod
    def color_for(cls, name):
        try:
            return cls[name.upper()].color
        except KeyError:
            return "white"


# --------------------------------------------------------------------------- #
# Serial console transport (Zephyr shell)
# --------------------------------------------------------------------------- #
def find_serial():
    from serial.tools import list_ports

    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            return p.device
    return None


class Device:
    def __init__(self, port):
        import serial

        self.s = serial.Serial(port, 115200, timeout=0.3)
        time.sleep(0.3)
        self.s.reset_input_buffer()
        self.s.write(b"\r\n")
        time.sleep(0.2)
        self.s.read(8192)

    def cmd(self, line, wait=2.0):
        self.s.reset_input_buffer()
        self.s.write((line + "\r\n").encode())
        self.s.flush()
        buf, end = "", time.time() + wait
        while time.time() < end:
            chunk = self.s.read(256).decode(errors="replace")
            buf += chunk
            if SHELL_PROMPT in buf:
                break
        buf = ANSI.sub("", buf)
        out = []
        for ln in buf.splitlines():
            ln = ln.strip()
            if ln and SHELL_PROMPT not in ln and ln != line and not ln.endswith(line):
                out.append(ln)
        return out


def open_device(port):
    port = port or find_serial()
    if not port:
        console.print("[red]No pwr-logger serial port found (USB 2fe3:0006). "
                      "Is it plugged in?[/]")
        sys.exit(2)
    try:
        return Device(port)
    except Exception as e:  # noqa: BLE001
        console.print(f"[red]Could not open {port}: {e}[/]")
        sys.exit(2)


# --------------------------------------------------------------------------- #
# Commands
# --------------------------------------------------------------------------- #
def parse_kv(lines):
    info = {}
    for ln in lines:
        if ":" in ln:
            k, v = ln.split(":", 1)
            info[k.strip()] = v.strip()
    return info


def cmd_status(dev):
    info = parse_kv(dev.cmd("pwr status"))
    state = info.get("state", "?")
    color = State.color_for(state)
    body = (f"[bold {color}]{state.upper()}[/]\n\n"
            f"time:   {info.get('time', '?')}\n"
            f"uptime: {info.get('uptime', '?')}")
    console.print(Panel(body, title="pwr-logger", border_style=color, expand=False))


def cmd_action(dev, name):
    out = dev.cmd(f"pwr {name}")
    console.print(f"[green]✓[/] {name}: " + (" ".join(out) if out else "ok"))


def cmd_time(dev, action, value):
    if action == "get":
        out = dev.cmd("pwr time get")
        console.print("  " + ("\n  ".join(out) if out else "(no response)"))
    else:
        epoch = int(time.time()) if (value in (None, "now")) else int(value, 0)
        out = dev.cmd(f"pwr time set {epoch}")
        console.print(f"[green]✓[/] time set: " + (" ".join(out) if out else f"{epoch}"))


def cmd_config(dev, action, key, value):
    if action == "list":
        t = Table(title="config", header_style="bold")
        t.add_column("key", style="cyan")
        t.add_column("value", justify="right")
        for ln in dev.cmd("pwr config list"):
            if "=" in ln:
                k, v = ln.split("=", 1)
                t.add_row(k.strip(), v.strip())
        console.print(t)
    elif action == "get":
        if not key:
            console.print("[red]usage: pwrctl config get <key>[/]")
            return
        console.print("  " + " ".join(dev.cmd(f"pwr config get {key}")))
    elif action == "set":
        if not key or value is None:
            console.print("[red]usage: pwrctl config set <key> <value>[/]")
            return
        out = dev.cmd(f"pwr config set {key} {value}")
        console.print(f"[green]✓[/] {key} = {value} " + (" ".join(out)))


# --------------------------------------------------------------------------- #
# BLE live telemetry
# --------------------------------------------------------------------------- #
def cmd_monitor():
    import asyncio
    import struct

    from bleak import BleakClient, BleakScanner
    from rich.live import Live

    latest = {}

    def render():
        # Fixed widths + a constant row set so the table never resizes frame-to-frame.
        t = Table(title="pwr-logger telemetry (BLE)", header_style="bold", width=52)
        t.add_column("field", style="cyan", width=20, no_wrap=True)
        t.add_column("value", justify="right", width=24, no_wrap=True)

        def dim(s):
            return f"[dim]{s}[/]"

        # INA228 precision channel (the trustworthy current/voltage/power)
        ina_i = f"[bold green]{latest['ina_i'] * 1000:8.2f} mA[/]" if "ina_i" in latest else dim("—")
        ina_v = f"{latest['ina_v']:.3f} V" if "ina_v" in latest else dim("—")
        ina_p = f"{latest['ina_p'] * 1000:.2f} mW" if "ina_p" in latest else dim("—")
        ina_t = f"{latest['ina_t']:.1f} °C" if "ina_t" in latest else dim("—")

        # fast channel (raw INA240->SAADC; secondary/diagnostic)
        if "avg" in latest and latest.get("n", 0) > 0:
            fc = SLOPE * latest["avg"] + INTER
            fast = dim(f"{fc * 1000:.0f} mA (code {latest['avg']:.0f})")
        else:
            fast = dim("—")

        if "state" in latest:
            try:
                s = State(latest["state"])
                st = f"[{s.color}]{s.name.lower()}[/]"
            except ValueError:
                st = "?"
        else:
            st = dim("…")
        batt = f"{latest['batt'] / 1000:.3f} V" if latest.get("batt") else dim("—")

        t.add_row("current (INA228)", ina_i)
        t.add_row("voltage (INA228)", ina_v)
        t.add_row("power (INA228)", ina_p)
        t.add_row("temp", ina_t)
        t.add_row("fast Ch (raw)", fast)
        t.add_row("fast rate", f"{latest.get('hz', 0):.1f} Hz")
        t.add_row("state", st)
        t.add_row("battery", batt)
        return t

    times = []
    last_rx = [0.0]
    STALL_S = 4.0   # no notifications this long while connected -> drop + reconnect

    def cb(_, data):
        d = bytes(data)
        if not d:
            return
        tag = d[0]
        last_rx[0] = time.monotonic()
        if tag == 0x01 and len(d) >= 15:        # FAST: raw fast-channel window
            _, t_ms, avg, mn, mx, n = struct.unpack("<BIfhhH", d[:15])
            now = time.monotonic()
            times.append(now)
            del times[:-20]
            hz = (len(times) - 1) / (times[-1] - times[0]) if len(times) > 1 else 0.0
            latest.update(avg=avg, mn=mn, mx=mx, n=n, hz=hz)
        elif tag == 0x02 and len(d) >= 17:      # SLOW: INA228 real units
            # Dropped t_ms (was 21 B, didn't fit default-MTU notify cap).
            _, vbus, i_a, p_w, temp = struct.unpack("<Bffff", d[:17])
            latest.update(ina_v=vbus, ina_i=i_a, ina_p=p_w, ina_t=temp)
        elif tag == 0x03 and len(d) >= 8:       # STATUS
            _, state, batt, drops = struct.unpack("<BBHI", d[:8])
            latest.update(state=state, batt=batt, drops=drops)

    def plain_line():
        if not latest:
            return
        parts = []
        if "ina_i" in latest:
            parts.append(f"INA228 {latest['ina_i'] * 1000:.2f} mA "
                         f"{latest.get('ina_v', 0):.3f} V")
        else:
            parts.append("INA228 —")
        if "avg" in latest and latest.get("n", 0) > 0:
            fc = SLOPE * latest["avg"] + INTER
            parts.append(f"fast {fc * 1000:.0f} mA")
        if "state" in latest:
            try:
                parts.append(State(latest["state"]).name.lower())
            except ValueError:
                pass
        if latest.get("batt"):
            parts.append(f"{latest['batt'] / 1000:.2f} Vbat")
        console.print("  ".join(parts))

    async def session():
        # Match on the advertised local name (BlueZ's find_device_by_name relies on
        # the cached Name property, which is often unset during active scan).
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: adv.local_name == "pwr-logger", timeout=12.0)
        if not dev:
            return
        async with BleakClient(dev) as c:
            console.print(f"[green]connected[/] {dev.address}")
            await c.start_notify(TELE_UUID, cb)
            last_rx[0] = time.monotonic()
            tty = console.is_terminal
            live = Live(render(), console=console, refresh_per_second=8) if tty else None
            if live:
                live.start()
            try:
                while c.is_connected:
                    await asyncio.sleep(0.12 if tty else 0.5)
                    if live:
                        live.update(render())
                    else:
                        plain_line()
                    if time.monotonic() - last_rx[0] > STALL_S:
                        console.print("[yellow]no telemetry — reconnecting[/]")
                        break
            finally:
                if live:
                    live.stop()

    async def run():
        console.print("[cyan]monitoring pwr-logger (auto-reconnect; Ctrl-C to quit)…[/]")
        while True:
            try:
                await session()
            except Exception as e:  # noqa: BLE001
                console.print(f"[yellow]link error: {e}[/]")
            latest.clear()
            await asyncio.sleep(1.0)   # brief pause before re-scan

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        console.print("\n[dim]bye[/]")


# --------------------------------------------------------------------------- #
# BLE control transport (write the control characteristic)
# --------------------------------------------------------------------------- #
def ble_send(payload):
    import asyncio
    from bleak import BleakClient, BleakScanner

    async def run():
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: adv.local_name == "pwr-logger", timeout=15.0)
        if not dev:
            console.print("[red]pwr-logger not found over BLE[/]")
            return False
        async with BleakClient(dev) as c:
            await c.write_gatt_char(CTRL_UUID, payload, response=False)
        return True

    return asyncio.run(run())


def ble_dispatch(a):
    import struct

    op = Op.__members__.get(a.cmd.upper())   # control commands map 1:1 to opcodes
    if op is not None and op is not Op.SETTIME:
        if ble_send(bytes([op])):
            console.print(f"[green]✓[/] {a.cmd} (BLE)")
    elif a.cmd == "time" and a.action == "set":
        epoch = int(time.time()) if a.value in (None, "now") else int(a.value, 0)
        if ble_send(bytes([Op.SETTIME]) + struct.pack("<I", epoch)):
            console.print(f"[green]✓[/] time set {epoch} (BLE)")
    else:
        console.print(f"[yellow]'{a.cmd}' isn't available over BLE — use serial, "
                      f"or 'pwrctl monitor' for live state/battery.[/]")


# --------------------------------------------------------------------------- #
# BLE keepalive: hold a connection open and ping at <interval> seconds
# --------------------------------------------------------------------------- #
def cmd_keepalive(interval):
    """Ping the device's keepalive opcode forever; defers auto-sleep on the
    device side so a long-running calibration / debug / monitor session
    can keep the device awake without flipping the autooff config.

    One persistent BLE connection (cheaper than reopen-per-ping), one
    write_gatt_char per interval. Ctrl-C to exit."""
    import asyncio
    from bleak import BleakClient, BleakScanner

    async def run():
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: adv.local_name == "pwr-logger", timeout=15.0)
        if not dev:
            console.print("[red]pwr-logger not found over BLE[/]")
            return 2
        console.print(f"[green]connected[/] {dev.address}; "
                      f"pinging keepalive every {interval} s. Ctrl-C to stop.")
        try:
            async with BleakClient(dev) as c:
                while True:
                    await c.write_gatt_char(CTRL_UUID,
                                            bytes([Op.KEEPALIVE]),
                                            response=False)
                    console.print(f"[dim]ping {time.strftime('%H:%M:%S')}[/]")
                    await asyncio.sleep(interval)
        except (asyncio.CancelledError, KeyboardInterrupt):
            return 0

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


# --------------------------------------------------------------------------- #
# BLE OTA: SMP-over-BLE image upload + test/confirm + reset
# --------------------------------------------------------------------------- #
#
# Wire protocol: Zephyr mcumgr SMP. Every request/response is an 8-byte
# header followed by a CBOR-encoded payload map. The same characteristic
# carries both writes (host -> device) and notifications (device -> host).
#
#   Byte 0:   OP (3 bits) | _ (3) | Version (2)
#               OP: 0=read, 1=read-rsp, 2=write, 3=write-rsp
#   Byte 1:   Flags (0)
#   Byte 2-3: BE16 length of CBOR payload that follows
#   Byte 4-5: BE16 group (img=1, os=0, ...)
#   Byte 6:   Sequence (matched in response)
#   Byte 7:   Command ID (img upload=1, os reset=5, ...)
#
# A single SMP packet can span multiple BLE writes -- the device
# reassembles based on the length field. To keep things simple and avoid
# device-side fragmentation bugs, we size each upload chunk so the whole
# packet fits in one write of (MTU - 3). Small packets (e.g. reset) are
# trivially one-write.
#
SMP_SVC_UUID  = "8d53dc1d-1db7-4cd3-868b-8a527460aa84"
SMP_CHAR_UUID = "da2e7828-fbce-4e01-ae9e-261174997c48"

SMP_OP_WRITE     = 2
SMP_GROUP_OS     = 0
SMP_GROUP_IMG    = 1
SMP_ID_OS_RESET  = 5
SMP_ID_IMG_STATE = 0   # GET/POST image list / test / confirm
SMP_ID_IMG_UPLOAD = 1
SMP_ID_IMG_ERASE = 5


def smp_pack(op, group, cmd_id, seq, cbor_body):
    hdr = bytes([
        (op & 0x07),                 # op + version=0
        0x00,                        # flags
        (len(cbor_body) >> 8) & 0xff,
        len(cbor_body) & 0xff,
        (group >> 8) & 0xff,
        group & 0xff,
        seq & 0xff,
        cmd_id & 0xff,
    ])
    return hdr + cbor_body


def smp_unpack(buf):
    if len(buf) < 8:
        return None
    op = buf[0] & 0x07
    length = (buf[2] << 8) | buf[3]
    group = (buf[4] << 8) | buf[5]
    seq = buf[6]
    cmd_id = buf[7]
    body = buf[8:8 + length]
    return op, group, seq, cmd_id, body


def cmd_ota(bin_path, *, confirm_now=False, reset=True, mark_only=False,
            keep_alive=False, keep_alive_interval=15.0):
    """Push <bin_path> to the device's secondary slot via BLE-SMP, mark for
    swap, optionally reset. Use the existing mcuboot.test/.confirm semantics:
    default = TEST (revert if new image doesn't confirm itself); --confirm-now
    = PERMANENT immediately."""
    import asyncio
    import hashlib
    import os

    import cbor2
    from bleak import BleakClient, BleakScanner

    data = open(bin_path, "rb").read()
    sha = hashlib.sha256(data).digest()
    console.print(f"[cyan]image {os.path.basename(bin_path)} "
                  f"({len(data)} bytes, sha {sha[:4].hex()}...)[/]")

    async def run():
        # Find device by advertised local name (matches cmd_monitor / firmware).
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: adv.local_name == "pwr-logger", timeout=10.0)
        if not dev:
            console.print("[red]pwr-logger not found via BLE[/]")
            return 2
        console.print(f"[green]connecting[/] {dev.address}")

        async with BleakClient(dev) as cli:
            # MTU: bleak's `mtu_size` is unreliable across versions
            # (read-only, may report a default 23 even when BlueZ has
            # actually negotiated up). The firmware advertises
            # CONFIG_BT_L2CAP_TX_MTU=247 and BlueZ auto-negotiates ATT
            # MTU on connect, so the link is high-MTU regardless of
            # what bleak reports. Pick a per-write size as large as
            # safely fits inside MTU-3=244 bytes to minimize per-chunk
            # round-trip overhead. If BlueZ rejects the write, the
            # write_gatt_char call will raise instead of failing
            # silently inside our chunk math.
            payload_max = 240
            # Per-chunk data size: payload_max - 8 (SMP hdr) - CBOR
            # framing overhead. First chunk carries the extra fields
            # (image=u8, len=u32, sha=32B = ~45B with bstr headers);
            # subsequent chunks are just {off:u32, data:bstr-N} =
            # ~15B. Reserve 50B to fit the first chunk and waste a
            # small amount on the rest. Bumping chunk_size from 112B
            # (the prior conservative value) to ~180B halves per-chunk
            # round-trips => ~1.8x upload speedup.
            chunk_overhead = 50
            chunk_size = max(payload_max - 8 - chunk_overhead, 64)
            console.print(f"[dim]payload_max={payload_max} chunk={chunk_size}B[/]")

            # SMP responses are fragmented across notifications when they
            # exceed the ATT MTU. The fragments arrive in order on the
            # same characteristic. Keep a single rolling buffer, drain it
            # of complete SMP packets (8-byte header + body of length
            # from the header), and dispatch each to its pending future.
            pending = {}
            rx_buf = bytearray()

            def on_notify(_h, value):
                rx_buf.extend(value)
                while len(rx_buf) >= 8:
                    body_len = (rx_buf[2] << 8) | rx_buf[3]
                    total = 8 + body_len
                    if len(rx_buf) < total:
                        return                    # wait for more frags
                    packet = bytes(rx_buf[:total])
                    del rx_buf[:total]
                    parsed = smp_unpack(packet)
                    if not parsed:
                        continue
                    op, group, seq, cmd_id, body = parsed
                    fut = pending.pop(seq, None)
                    if fut and not fut.done():
                        fut.set_result((op, group, cmd_id, body))

            # Register the notify handler exactly once. (Earlier versions
            # of this code did a start/stop/start dance to "upgrade" from
            # a placeholder lambda to on_notify; on some BlueZ versions
            # that second start_notify silently lost the notification
            # subscription and every request timed out.)
            await cli.start_notify(SMP_CHAR_UUID, on_notify)

            loop = asyncio.get_running_loop()
            seq = 0

            async def request(op, group, cmd_id, payload, timeout=15.0):
                nonlocal seq
                this_seq = seq
                seq = (seq + 1) & 0xff
                body = cbor2.dumps(payload)
                packet = smp_pack(op, group, cmd_id, this_seq, body)
                fut = loop.create_future()
                pending[this_seq] = fut
                # SMP supports fragmented writes; for simplicity insist that
                # the whole packet fits in one write at the current MTU.
                if len(packet) > payload_max:
                    # Fall back to fragmented writes (split at byte boundary;
                    # the device reassembles by length).
                    for i in range(0, len(packet), payload_max):
                        await cli.write_gatt_char(
                            SMP_CHAR_UUID, packet[i:i + payload_max],
                            response=False)
                else:
                    await cli.write_gatt_char(
                        SMP_CHAR_UUID, packet, response=False)
                try:
                    op_r, _g, _id, rbody = await asyncio.wait_for(
                        fut, timeout=timeout)
                except asyncio.TimeoutError:
                    pending.pop(this_seq, None)
                    raise
                if not rbody:
                    return {}
                return cbor2.loads(rbody)

            # ---- inspect slot state ----
            console.print("[cyan]image list (before)...[/]")
            slot1_hash_before = None
            try:
                ls = await request(0, SMP_GROUP_IMG, SMP_ID_IMG_STATE,
                                   {}, timeout=10.0)
                images = ls.get("images", []) if isinstance(ls, dict) else []
                for im in images:
                    flags = []
                    for k in ("active", "confirmed", "pending", "permanent", "bootable"):
                        if im.get(k):
                            flags.append(k)
                    h = im.get("hash", b"")
                    console.print(
                        f"  slot={im.get('slot')}  v={im.get('version','?')}  "
                        f"sha={h[:4].hex() if h else '-'}  [{','.join(flags) or '-'}]"
                    )
                    if im.get("slot") == 1 and h:
                        slot1_hash_before = h
            except Exception as e:
                console.print(f"[yellow]image list: {type(e).__name__}: {e}[/]")

            if mark_only:
                if not slot1_hash_before:
                    console.print("[red]--mark: slot 1 empty, nothing to mark[/]")
                    return 9
                console.print("[cyan]marking slot 1 for swap (using existing image)...[/]")
                mark_payload = {"hash": slot1_hash_before,
                                "confirm": bool(confirm_now)}
                mark_resp = await request(SMP_OP_WRITE, SMP_GROUP_IMG,
                                          SMP_ID_IMG_STATE, mark_payload)
                console.print(f"[dim]mark response: {mark_resp}[/]")
                rc = mark_resp.get("rc") if isinstance(mark_resp, dict) else None
                if isinstance(rc, int) and rc != 0:
                    console.print(f"[red]mark error rc={rc}[/]")
                    return 4
                mode = "PERMANENT" if confirm_now else "TEST"
                console.print(f"[green]marked as {mode}[/]")
                if reset:
                    console.print("[cyan]resetting...[/]")
                    try:
                        await request(SMP_OP_WRITE, SMP_GROUP_OS,
                                      SMP_ID_OS_RESET, {}, timeout=2.0)
                    except asyncio.TimeoutError:
                        pass
                    console.print("[green]reset issued -- swap on boot[/]")
                return 0

            # NOTE: no explicit erase. The firmware has
            # CONFIG_IMG_ERASE_PROGRESSIVELY=y, so the upload below
            # erases each flash page just before writing it. The
            # one-shot erase command is broken on this device: the
            # ~5 s of blocking flash work exceeds the BT supervision
            # timeout (CONFIG_BT_PERIPHERAL_PREF_TIMEOUT=42 = 420 ms),
            # so the link drops before the response can come back.

            # ---- upload ----
            total = len(data)
            off = 0
            t0 = time.monotonic()
            console.print("[cyan]uploading...[/]")
            while off < total:
                this = min(chunk_size, total - off)
                payload = {
                    "off": off,
                    "data": data[off:off + this],
                }
                if off == 0:
                    payload["image"] = 0
                    payload["len"] = total
                    payload["sha"] = sha
                resp = await request(SMP_OP_WRITE, SMP_GROUP_IMG,
                                     SMP_ID_IMG_UPLOAD, payload,
                                     timeout=20.0)
                if isinstance(resp.get("rc"), int) and resp["rc"] != 0:
                    console.print(f"[red]upload error rc={resp['rc']} at off={off}[/]")
                    return 3
                next_off = resp.get("off", off + this)
                off = next_off
                pct = (off * 100) // total
                rate = off / (time.monotonic() - t0 + 1e-9) / 1024
                console.print(
                    f"  {off:>7d}/{total} ({pct:3d}%) {rate:6.1f} KB/s",
                    end="\r", highlight=False)
            console.print()
            elapsed = time.monotonic() - t0
            console.print(f"[green]upload done[/] in {elapsed:.1f} s "
                          f"({total/elapsed/1024:.1f} KB/s)")

            # ---- look up slot 1's hash for the mark ----
            # MCUboot identifies images by the SHA-256 TLV embedded in the
            # image header trailer, NOT the SHA of the .bin file we
            # uploaded. Ask the device for the hash it actually computed
            # for slot 1, then use that for the test/confirm request.
            console.print("[cyan]image list (after upload)...[/]")
            slot1_hash = None
            try:
                ls = await request(0, SMP_GROUP_IMG, SMP_ID_IMG_STATE,
                                   {}, timeout=10.0)
                for im in ls.get("images", []) if isinstance(ls, dict) else []:
                    h = im.get("hash", b"")
                    flags = [k for k in ("active", "confirmed", "pending",
                                         "permanent", "bootable") if im.get(k)]
                    console.print(
                        f"  slot={im.get('slot')}  v={im.get('version','?')}  "
                        f"sha={h[:4].hex() if h else '-'}  [{','.join(flags) or '-'}]")
                    if im.get("slot") == 1 and h:
                        slot1_hash = h
            except Exception as e:
                console.print(f"[red]image list (after): {type(e).__name__}: {e}"
                              f" -- can't mark without hash[/]")
                return 7

            if not slot1_hash:
                console.print("[red]slot 1 hash not reported -- upload may have failed silently[/]")
                return 8

            # ---- mark for swap ----
            console.print("[cyan]marking for swap...[/]")
            mark_payload = {"hash": slot1_hash, "confirm": bool(confirm_now)}
            mark_resp = await request(SMP_OP_WRITE, SMP_GROUP_IMG,
                                      SMP_ID_IMG_STATE, mark_payload)
            console.print(f"[dim]mark response: {mark_resp}[/]")
            if isinstance(mark_resp.get("rc"), int) and mark_resp["rc"] != 0:
                console.print(f"[red]mark error rc={mark_resp['rc']}[/]")
                return 4
            mode = "PERMANENT" if confirm_now else "TEST (will revert unless image confirms)"
            console.print(f"[green]marked as {mode}[/]")

            # ---- reset ----
            if reset:
                console.print("[cyan]resetting...[/]")
                try:
                    await request(SMP_OP_WRITE, SMP_GROUP_OS,
                                  SMP_ID_OS_RESET, {}, timeout=2.0)
                except asyncio.TimeoutError:
                    # Reset response is best-effort; the device may not get
                    # a chance to send it before reboot. That's normal.
                    pass
                console.print("[green]reset issued -- device will swap on boot[/]")

        return 0

    rc = asyncio.run(run())
    if rc:
        sys.exit(rc)

    # ---- post-reset keepalive ----
    # The connection above closed when sys_reboot fired. If the operator
    # wants to interact with the new image (verify build tag, etc.)
    # without it dropping to LS_IDLE first, reconnect once it's
    # re-advertising and start the keepalive loop. Best-effort: if it
    # doesn't come back in 30 s we give up and let the user drive.
    if reset and keep_alive:
        import asyncio as _aio

        async def _post_reset_keepalive():
            from bleak import BleakClient, BleakScanner
            console.print(
                f"[cyan]waiting for device to come back up for keepalive...[/]")
            deadline = time.monotonic() + 30.0
            dev = None
            while time.monotonic() < deadline:
                dev = await BleakScanner.find_device_by_filter(
                    lambda d, adv: adv.local_name == "pwr-logger",
                    timeout=5.0)
                if dev:
                    break
            if not dev:
                console.print(
                    "[yellow]device didn't re-advertise within 30 s -- giving up "
                    "(may already be sleeping, or boot took longer than expected). "
                    "Run `pwrctl keepalive` manually when it comes back.[/]")
                return
            console.print(f"[green]reconnected[/] {dev.address}; "
                          f"pinging every {keep_alive_interval} s. Ctrl-C to stop.")
            try:
                async with BleakClient(dev) as c:
                    while True:
                        await c.write_gatt_char(CTRL_UUID,
                                                bytes([Op.KEEPALIVE]),
                                                response=False)
                        console.print(
                            f"[dim]ping {time.strftime('%H:%M:%S')}[/]")
                        await _aio.sleep(keep_alive_interval)
            except (_aio.CancelledError, KeyboardInterrupt):
                pass

        try:
            _aio.run(_post_reset_keepalive())
        except KeyboardInterrupt:
            pass


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(prog="pwrctl", description="pwr-logger host CLI")
    ap.add_argument("--port", help="serial port (default: auto-detect 2fe3:0006)")
    ap.add_argument("--ble", action="store_true",
                    help="send control commands over BLE instead of serial")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")
    for c in ("log", "stop", "rotate", "offload", "resume", "poweroff"):
        sub.add_parser(c)
    tp = sub.add_parser("time")
    tp.add_argument("action", choices=["get", "set"])
    tp.add_argument("value", nargs="?")
    cp = sub.add_parser("config")
    cp.add_argument("action", choices=["list", "get", "set"])
    cp.add_argument("key", nargs="?")
    cp.add_argument("value", nargs="?")
    sub.add_parser("monitor")

    kap = sub.add_parser("keepalive",
                         help="hold a BLE connection open and ping the device's "
                              "keepalive opcode every <interval> s, deferring "
                              "auto-sleep for as long as the loop runs")
    kap.add_argument("--interval", type=float, default=15.0,
                     help="seconds between pings (default 15)")

    op = sub.add_parser("ota", help="push a signed .bin firmware over BLE-SMP")
    op.add_argument("bin", help="path to zephyr.signed.bin")
    op.add_argument("--confirm-now", action="store_true",
                    help="mark as PERMANENT (no revert) instead of TEST")
    op.add_argument("--no-reset", action="store_true",
                    help="skip the post-upload reset (leave pending swap)")
    op.add_argument("--mark", action="store_true",
                    help="skip erase+upload; mark whatever is already in slot 1 "
                         "(diagnostic; use when a prior upload landed but mark "
                         "failed)")
    op.add_argument("--keep-alive", action="store_true",
                    help="after the post-OTA reset, reconnect and run the "
                         "keepalive loop so the swapped image doesn't drop "
                         "into LS_IDLE before you can interact with it")
    op.add_argument("--keep-alive-interval", type=float, default=15.0,
                    help="seconds between keepalive pings (default 15, "
                         "only with --keep-alive)")

    a = ap.parse_args()

    if a.cmd == "monitor":
        cmd_monitor()
        return
    if a.cmd == "keepalive":
        cmd_keepalive(a.interval)
        return
    if a.cmd == "ota":
        cmd_ota(a.bin, confirm_now=a.confirm_now, reset=not a.no_reset,
                mark_only=a.mark, keep_alive=a.keep_alive,
                keep_alive_interval=a.keep_alive_interval)
        return
    if a.ble:
        ble_dispatch(a)
        return
    dev = open_device(a.port)
    if a.cmd == "status":
        cmd_status(dev)
    elif a.cmd in ("log", "stop", "rotate", "offload", "resume", "poweroff"):
        cmd_action(dev, a.cmd)
    elif a.cmd == "time":
        cmd_time(dev, a.action, a.value)
    elif a.cmd == "config":
        cmd_config(dev, a.action, a.key, a.value)


if __name__ == "__main__":
    main()
