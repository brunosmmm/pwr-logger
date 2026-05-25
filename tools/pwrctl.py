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

STATE_COLOR = {"logging": "green", "stopped": "yellow", "offload": "cyan", "ship": "red"}


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
    color = STATE_COLOR.get(state, "white")
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
                        console.print(f"I={cur * 1000:7.1f} mA  min/max code="
                                      f"{latest['mn']}/{latest['mx']}  n={latest['n']}  "
                                      f"{latest.get('hz', 0):.1f} Hz")

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        console.print("\n[dim]bye[/]")


# --------------------------------------------------------------------------- #
def main():
    ap = argparse.ArgumentParser(prog="pwrctl", description="pwr-logger host CLI")
    ap.add_argument("--port", help="serial port (default: auto-detect 2fe3:0006)")
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
