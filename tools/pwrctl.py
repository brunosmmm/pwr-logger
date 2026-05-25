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
        t = Table(title="pwr-logger telemetry (BLE)", header_style="bold")
        t.add_column("field", style="cyan")
        t.add_column("value", justify="right")
        if not latest:
            t.add_row("status", "[dim]waiting for notifications…[/]")
            return t
        cur = SLOPE * latest["avg"] + INTER
        cur_min = SLOPE * latest["mn"] + INTER
        cur_max = SLOPE * latest["mx"] + INTER
        t.add_row("current (avg)", f"[bold green]{cur * 1000:8.1f} mA[/]")
        t.add_row("min / max", f"{cur_min * 1000:.1f} / {cur_max * 1000:.1f} mA")
        t.add_row("avg code", f"{latest['avg']:.1f}")
        t.add_row("samples/window", str(latest["n"]))
        t.add_row("rate", f"{latest.get('hz', 0):.1f} Hz")
        if "state" in latest:
            try:
                s = State(latest["state"])
                name, col = s.name.lower(), s.color
            except ValueError:
                name, col = "?", "white"
            t.add_row("state", f"[{col}]{name}[/]")
            t.add_row("battery", f"{latest['batt'] / 1000:.3f} V" if latest["batt"] else "—")
        return t

    async def run():
        console.print("[cyan]scanning for pwr-logger…[/]")
        # Match on the advertised local name (BlueZ's find_device_by_name relies
        # on the cached Name property, which is often unset during active scan).
        dev = await BleakScanner.find_device_by_filter(
            lambda d, adv: adv.local_name == "pwr-logger", timeout=15.0)
        if not dev:
            console.print("[red]pwr-logger not found over BLE[/]")
            return
        console.print(f"[green]connected[/] {dev.address}  (Ctrl-C to quit)")
        times = []

        def cb(_, data):
            d = bytes(data)
            if len(d) < 24:
                return
            ts, rms, avg, mn, mx, n = struct.unpack("<QffhhI", d[:24])
            now = time.monotonic()
            times.append(now)
            del times[:-20]
            hz = (len(times) - 1) / (times[-1] - times[0]) if len(times) > 1 else 0.0
            latest.update(avg=avg, mn=mn, mx=mx, n=n, rms=rms, hz=hz)
            if len(d) >= 27:   # extended: + u16 batt_mv, u8 state
                batt, state = struct.unpack("<HB", d[24:27])
                latest.update(batt=batt, state=state)

        async with BleakClient(dev) as c:
            await c.start_notify(TELE_UUID, cb)
            if console.is_terminal:
                with Live(render(), console=console, refresh_per_second=8) as live:
                    while True:
                        await asyncio.sleep(0.12)
                        live.update(render())
            else:
                # piped/logged: emit a plain line per update
                while True:
                    await asyncio.sleep(0.5)
                    if latest:
                        cur = SLOPE * latest["avg"] + INTER
                        extra = ""
                        if "state" in latest:
                            try:
                                extra = (f"  {State(latest['state']).name.lower()}"
                                         f"  {latest['batt'] / 1000:.2f}V")
                            except ValueError:
                                pass
                        console.print(f"I={cur * 1000:7.1f} mA  min/max code="
                                      f"{latest['mn']}/{latest['mx']}  n={latest['n']}  "
                                      f"{latest.get('hz', 0):.1f} Hz{extra}")

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

    a = ap.parse_args()

    if a.cmd == "monitor":
        cmd_monitor()
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
