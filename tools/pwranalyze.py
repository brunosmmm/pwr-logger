#!/usr/bin/env python3
"""
pwranalyze -- TDMS file inspector + analysis helper for pwr-logger sessions.

The on-card TDMS schema (firmware-side: src/tdms.c):

  root properties: pwrlog_format_version, master_clock_hz, ina240_*,
                   ina228_shunt_ohms, adc_vfull_scale_v, adc_bits,
                   session_start_utc, time_synced
  /fast/i        : float64 (NI linear scale applied), 20 kSPS, wf_increment=50us
  /slow/t        : datetime64[us]  (NI timestamp -- one per slow record)
  /slow/vbus     : V    (NI linear scale on the channel)
  /slow/vshunt   : V
  /slow/current  : A
  /slow/power    : W
  /slow/temp     : degC

Requires pwrlog_format_version >= 2 (slow channels carry their own NI
Linear-Scale properties). Older files would need to be re-recorded.

`Session.load(path)` returns SI-scaled numpy arrays for both channels with
elapsed-seconds time axes (relative to session_start_utc) so fast and slow
align. The subcommands wrap that with rich-formatted output / plotting /
export.

Usage:
  pwranalyze info FILE
  pwranalyze stats FILE [--window START:DURATION]
  pwranalyze plot  FILE [--window START:DURATION] [--out PNG]
  pwranalyze fft   FILE [--window START:DURATION] [--out PNG]
  pwranalyze export FILE -o OUT.{npz,csv}
"""
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

try:
    from nptdms import TdmsFile
except ImportError:
    sys.exit("missing nptdms -- pip install -r tools/requirements.txt")

try:
    from rich.console import Console
    from rich.panel import Panel
    from rich.table import Table
except ImportError:
    sys.exit("missing rich -- pip install -r tools/requirements.txt")

console = Console()

MIN_FORMAT_VERSION = 2


# ---- core data model ------------------------------------------------------
@dataclass
class Session:
    """Loaded + SI-scaled view of a pwr-logger TDMS file.

    Time axes for both channels are in elapsed seconds since the session's
    first fast sample (so fast and slow share an origin). Wall-clock anchor
    is `session_start_utc` (numpy datetime64 if synced, else None)."""
    path: Path
    props: dict = field(default_factory=dict)
    session_start: np.datetime64 | None = None
    time_synced: bool = False
    # Fast channel
    fast_i: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float64))
    fs: float = 0.0
    # Slow channel (SI units)
    slow_t: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float64))   # seconds
    slow_v: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float64))   # volts
    slow_i: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float64))   # amps
    slow_p: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float64))   # watts
    slow_temp: np.ndarray = field(default_factory=lambda: np.array([], dtype=np.float64))   # °C

    @classmethod
    def load(cls, path: str | Path) -> "Session":
        path = Path(path)
        tdms = TdmsFile.read(str(path))
        s = cls(path=path, props=dict(tdms.properties))
        sus = s.props.get("session_start_utc")
        s.session_start = np.datetime64(sus.replace("Z", ""), "us") if sus else None
        s.time_synced = bool(s.props.get("time_synced", 0))

        # Fast channel: float64, already scaled to amps by npTDMS via the NI
        # linear-scale properties baked in at write time.
        ch_fast = tdms["fast"]["i"]
        s.fast_i = ch_fast[:]
        s.fs = 1.0 / float(ch_fast.properties["wf_increment"])

        ver = int(s.props.get("pwrlog_format_version", 0))
        if ver < MIN_FORMAT_VERSION:
            sys.exit(f"{path}: pwrlog_format_version={ver}, need >= "
                     f"{MIN_FORMAT_VERSION}. Re-record with current firmware.")

        # Slow time axis: relative to session_start, in seconds (float64).
        # Slow channel values: NI Linear scales on each channel mean a normal
        # read returns float64 in SI units already.
        slow_t_dt = tdms["slow"]["t"][:]   # datetime64[us] absolute
        if s.session_start is not None and slow_t_dt.size:
            delta = (slow_t_dt - s.session_start).astype("timedelta64[us]").astype(np.int64)
            s.slow_t = delta * 1e-6
        else:
            s.slow_t = np.arange(slow_t_dt.size, dtype=np.float64) / 20.0

        s.slow_v    = tdms["slow"]["vbus"][:].astype(np.float64)
        s.slow_i    = tdms["slow"]["current"][:].astype(np.float64)
        s.slow_p    = tdms["slow"]["power"][:].astype(np.float64)
        s.slow_temp = tdms["slow"]["temp"][:].astype(np.float64)
        return s

    @property
    def fast_duration_s(self) -> float:
        return len(self.fast_i) / self.fs if self.fs > 0 else 0.0

    @property
    def fast_t(self) -> np.ndarray:
        """Elapsed-seconds time axis for the fast channel (computed on demand
        rather than stored -- saves 8 bytes * N_fast)."""
        return np.arange(len(self.fast_i), dtype=np.float64) / self.fs

    def slice_fast(self, t0: float, t1: float) -> tuple[np.ndarray, np.ndarray]:
        """Return (t_axis, samples) for fast channel in window [t0, t1) sec."""
        i0 = max(0, int(t0 * self.fs))
        i1 = min(len(self.fast_i), int(t1 * self.fs))
        return self.fast_t[i0:i1], self.fast_i[i0:i1]

    def slice_slow(self, t0: float, t1: float) -> tuple[np.ndarray, ...]:
        """Return (t, v, i, p, temp) for slow channel in window [t0, t1) sec.

        Truncates to the shortest channel -- npTDMS occasionally reports
        slightly different lengths for the timestamp vs the value channels
        when the file's last segment is partial (firmware writer flushed
        mid-record). All channels share index 0..N-1 alignment, so
        truncating to min(len) keeps them aligned."""
        n = min(self.slow_t.size, self.slow_v.size, self.slow_i.size,
                self.slow_p.size, self.slow_temp.size)
        t  = self.slow_t [:n]
        mask = (t >= t0) & (t < t1)
        return (t[mask], self.slow_v[:n][mask], self.slow_i[:n][mask],
                self.slow_p[:n][mask], self.slow_temp[:n][mask])


def parse_window(spec: str | None, total_s: float) -> tuple[float, float]:
    """Parse `START:DURATION` (sec) into (t0, t1). Default: full file."""
    if spec is None:
        return 0.0, total_s
    parts = spec.split(":")
    if len(parts) != 2:
        sys.exit(f"--window must be START:DURATION (sec); got {spec!r}")
    t0 = float(parts[0])
    dur = float(parts[1])
    return t0, min(t0 + dur, total_s)


# ---- subcommands ----------------------------------------------------------
def cmd_info(s: Session) -> None:
    p = Table.grid(padding=(0, 2))
    p.add_column(style="cyan")
    p.add_column()
    for k in ("session_start_utc", "time_synced", "pwrlog_format_version",
              "master_clock_hz", "adc_bits", "adc_vfull_scale_v",
              "ina240_gain", "ina240_vref_v", "ina240_shunt_ohms",
              "ina228_shunt_ohms"):
        if k in s.props:
            p.add_row(k, str(s.props[k]))
    console.print(Panel(p, title=f"root properties · {s.path.name}",
                        border_style="cyan", expand=False))

    ch = Table(title="channels", header_style="bold")
    ch.add_column("path", style="cyan")
    ch.add_column("rate", justify="right")
    ch.add_column("samples", justify="right")
    ch.add_column("duration", justify="right")
    ch.add_row("/fast/i", f"{s.fs:.0f} Hz", f"{len(s.fast_i):,}",
               f"{s.fast_duration_s:.3f} s")
    if len(s.slow_t):
        slow_dur = s.slow_t[-1] - s.slow_t[0]
        slow_rate = len(s.slow_t) / slow_dur if slow_dur > 0 else 0
        ch.add_row("/slow/{t,vbus,vshunt,current,power,temp}",
                   f"{slow_rate:.2f} Hz", f"{len(s.slow_t):,}",
                   f"{slow_dur:.3f} s")
    console.print(ch)


def _stat_row(name: str, x: np.ndarray, unit: str) -> tuple[str, ...]:
    if x.size == 0:
        return name, "—", "—", "—", "—", "—"
    rms = float(np.sqrt(np.mean(x * x)))
    return (name,
            f"{x.mean():.4g} {unit}",
            f"{rms:.4g} {unit}",
            f"{x.min():.4g} {unit}",
            f"{x.max():.4g} {unit}",
            f"{x.std():.4g} {unit}")


def cmd_stats(s: Session, window: str | None) -> None:
    t0, t1 = parse_window(window, s.fast_duration_s)
    t_fast, i_fast = s.slice_fast(t0, t1)
    t_slow, v, i, p, temp = s.slice_slow(t0, t1)

    title = f"stats · t ∈ [{t0:.3f}, {t1:.3f}) s ({t1-t0:.3f} s)"
    tbl = Table(title=title, header_style="bold")
    for col in ("channel", "mean", "rms", "min", "max", "std"):
        tbl.add_column(col, justify="right" if col != "channel" else "left",
                       style="cyan" if col == "channel" else None)
    tbl.add_row(*_stat_row("fast i (INA240)", i_fast, "A"))
    tbl.add_row(*_stat_row("slow vbus  (INA228)", v, "V"))
    tbl.add_row(*_stat_row("slow current (INA228)", i, "A"))
    tbl.add_row(*_stat_row("slow power   (INA228)", p, "W"))
    tbl.add_row(*_stat_row("slow temp    (INA228)", temp, "°C"))
    console.print(tbl)


def _ensure_mpl():
    try:
        import matplotlib
        matplotlib.use("Agg")    # default headless; user can override
        import matplotlib.pyplot as plt
        return plt
    except ImportError:
        sys.exit("matplotlib not installed -- pip install matplotlib")


def cmd_plot(s: Session, window: str | None, out: str | None) -> None:
    plt = _ensure_mpl()
    t0, t1 = parse_window(window, s.fast_duration_s)
    t_fast, i_fast = s.slice_fast(t0, t1)
    t_slow, v, i, p, temp = s.slice_slow(t0, t1)

    # Decimate fast for plotting if it's huge (millions of points kills mpl).
    target_points = 200_000
    if len(t_fast) > target_points:
        step = len(t_fast) // target_points
        t_fast = t_fast[::step]
        i_fast = i_fast[::step]
        console.print(f"[yellow]fast channel decimated 1:{step} for plotting "
                      f"({len(t_fast):,} points)[/]")

    fig, axes = plt.subplots(3, 1, sharex=True, figsize=(12, 8))
    axes[0].plot(t_fast, i_fast * 1000, "-", lw=0.5)
    axes[0].set_ylabel("fast I (mA)")
    axes[0].grid(alpha=0.3)
    if len(t_slow):
        axes[1].plot(t_slow, i * 1000, ".-", lw=0.8, ms=3)
    axes[1].set_ylabel("INA228 I (mA)")
    axes[1].grid(alpha=0.3)
    if len(t_slow):
        axes[2].plot(t_slow, v, ".-", lw=0.8, ms=3, label="V")
        ax2 = axes[2].twinx()
        ax2.plot(t_slow, p * 1000, ".-", lw=0.8, ms=3, color="C1", label="P")
        ax2.set_ylabel("P (mW)", color="C1")
    axes[2].set_ylabel("V (V)")
    axes[2].set_xlabel("t (s, relative to session_start)")
    axes[2].grid(alpha=0.3)
    fig.suptitle(f"{s.path.name}   ·   "
                 f"{s.props.get('session_start_utc', '?')}   ·   "
                 f"{t0:.2f}–{t1:.2f} s")
    fig.tight_layout()

    if out is None:
        out = str(s.path.with_suffix(".png"))
    fig.savefig(out, dpi=120)
    console.print(f"[green]✓[/] wrote {out}")


def cmd_fft(s: Session, window: str | None, out: str | None) -> None:
    plt = _ensure_mpl()
    t0, t1 = parse_window(window, s.fast_duration_s)
    t_fast, i_fast = s.slice_fast(t0, t1)
    if len(i_fast) < 8:
        sys.exit("not enough fast samples in window for FFT")
    x = i_fast - i_fast.mean()                      # DC-remove
    win = np.hanning(len(x))
    X = np.fft.rfft(x * win)
    f = np.fft.rfftfreq(len(x), d=1.0 / s.fs)
    mag_db = 20 * np.log10(np.abs(X) / (len(x) / 2) + 1e-15)

    fig, ax = plt.subplots(figsize=(12, 5))
    ax.semilogx(f[1:], mag_db[1:], lw=0.7)
    ax.set_xlabel("frequency (Hz)")
    ax.set_ylabel("magnitude (dB, ref 1 A)")
    ax.set_title(f"fast-channel spectrum   ·   {s.path.name}   ·   "
                 f"{t0:.2f}–{t1:.2f} s   ·   N={len(x):,}")
    ax.grid(which="both", alpha=0.3)
    fig.tight_layout()

    if out is None:
        out = str(s.path.with_suffix(".fft.png"))
    fig.savefig(out, dpi=120)
    console.print(f"[green]✓[/] wrote {out}")


def cmd_export(s: Session, out: str) -> None:
    out_path = Path(out)
    if out_path.suffix == ".npz":
        np.savez_compressed(
            out_path,
            fs=s.fs,
            fast_i=s.fast_i,
            slow_t=s.slow_t,
            slow_v=s.slow_v,
            slow_i=s.slow_i,
            slow_p=s.slow_p,
            slow_temp=s.slow_temp,
            session_start_utc=str(s.props.get("session_start_utc", "")),
            time_synced=int(s.time_synced),
        )
    elif out_path.suffix == ".csv":
        # Two files: <stem>_fast.csv and <stem>_slow.csv. The fast file is
        # huge (~20 MB / minute as CSV) -- this is by design; if you want
        # compact use NPZ.
        fast_csv = out_path.with_name(out_path.stem + "_fast.csv")
        slow_csv = out_path.with_name(out_path.stem + "_slow.csv")
        # fast
        with open(fast_csv, "w") as f:
            f.write("t_s,i_A\n")
            t_fast = s.fast_t
            for ts, val in zip(t_fast, s.fast_i):
                f.write(f"{ts:.6f},{val:.6e}\n")
        # slow
        with open(slow_csv, "w") as f:
            f.write("t_s,v_V,i_A,p_W,temp_C\n")
            for ts, v, i, p, t in zip(s.slow_t, s.slow_v, s.slow_i,
                                      s.slow_p, s.slow_temp):
                f.write(f"{ts:.6f},{v:.6e},{i:.6e},{p:.6e},{t:.4f}\n")
        out = f"{fast_csv}, {slow_csv}"
    else:
        sys.exit("output must end in .npz or .csv")
    console.print(f"[green]✓[/] wrote {out}")


# ---- CLI ------------------------------------------------------------------
def main() -> None:
    ap = argparse.ArgumentParser(prog="pwranalyze", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    for name in ("info", "stats", "plot", "fft", "export"):
        sp = sub.add_parser(name)
        sp.add_argument("file", help="path to a pwr-logger .tdms file")
        if name in ("stats", "plot", "fft"):
            sp.add_argument("--window", metavar="START:DURATION",
                            help="time window in seconds, e.g. 30:5 = 5 s starting at t=30 s")
        if name in ("plot", "fft"):
            sp.add_argument("--out", help="output PNG path (default: alongside .tdms)")
        if name == "export":
            sp.add_argument("-o", "--out", required=True,
                            help="output path (.npz or .csv)")
    a = ap.parse_args()

    s = Session.load(a.file)
    if a.cmd == "info":
        cmd_info(s)
    elif a.cmd == "stats":
        cmd_stats(s, a.window)
    elif a.cmd == "plot":
        cmd_plot(s, a.window, a.out)
    elif a.cmd == "fft":
        cmd_fft(s, a.window, a.out)
    elif a.cmd == "export":
        cmd_export(s, a.out)


if __name__ == "__main__":
    main()
