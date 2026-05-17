#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy>=2.0"]
# ///
"""
ppk_analysis.py  –  Nordic PPK2 Power Analyser
===============================================
Analyses current-consumption recordings from the Nordic Power Profiler Kit II.
Supports both export formats:

  .csv   — PPK2 text export
              Timestamp(ms),Current(uA),D0-D7
              570,0.037,00000000

  .ppk2  — PPK2 native binary format (ZIP archive)
              metadata.json  – samplesPerSecond, startSystemTime, formatVersion
              session.raw    – stride-6 layout: float32 (µA) + uint16 (8× 2-bit DIN)
              minimap.raw    – downsampled overview (not used here)

              DIN uint16 encoding (2 bits per channel):
                  D0 → bits[9:8],  D1 → bits[11:10], D2 → bits[13:12], D3 → bits[15:14]
                  D4 → bits[1:0],  D5 → bits[3:2],   D6 → bits[5:4],   D7 → bits[7:6]
                  pair value: 0b01 = LOW, 0b10 = HIGH, 0b11 = X (metastable, forward-filled)

Features
--------
* Handles 100 S/s … 100 kS/s, files up to ~100 MB (loaded fully into RAM via numpy)
* Automatic boot-phase detection & exclusion from battery estimates
* Per-channel HIGH-phase analysis: avg, peak, energy per pulse + summary
* Overall & steady-state current statistics
* Battery life estimate: CR2032 / 2×AA / 2×AAA
* Optional per-second profile table (--per-second)

Usage
-----
    uv run scripts/ppk_analysis.py data/recording.ppk2
    uv run scripts/ppk_analysis.py data/recording.csv
    uv run scripts/ppk_analysis.py data/recording.ppk2 --per-second
    uv run scripts/ppk_analysis.py data/recording.csv  --no-boot-exclusion
"""

import argparse
import csv
import json
import sys
import zipfile
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------------------
# Battery specs: (display label, capacity_mAh, footnote)
# ---------------------------------------------------------------------------
BATTERIES = [
    ("CR2032   1× 3.0 V",   230,  "typical Li coin cell"),
    ("2× AA    3.0 V ser.", 2500,  "2× 1.5 V alkaline in series"),
    ("2× AAA   3.0 V ser.", 1200,  "2× 1.5 V alkaline in series"),
]

COL_W = 72  # output width

# PPK2 binary format: bit offsets in the uint16 DIN word for each channel D0..D7
# Encoding: 0b01 = LOW, 0b10 = HIGH, 0b11 = X/metastable
_DIN_BIT_SHIFTS = [8, 10, 12, 14, 0, 2, 4, 6]  # D0, D1, D2, D3, D4, D5, D6, D7


# ===========================================================================
# I/O helpers
# ===========================================================================

def load_csv(path: Path):
    """
    Read PPK2 CSV and return numpy arrays:
        timestamps  – int64,   milliseconds
        currents    – float64, microamperes
        din_matrix  – uint8 array (n × 8), one column per digital channel

    Rows with missing or malformed values are silently skipped.
    Metastable 'X' states are forward-filled from the last valid bit value.
    """
    ts_list, i_list, din_list = [], [], []
    with path.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            try:
                t = int(row["Timestamp(ms)"])
                i = float(row["Current(uA)"])
                d = row["D0-D7"]
            except (KeyError, ValueError, TypeError):
                continue
            ts_list.append(t)
            i_list.append(i)
            din_list.append(d)

    timestamps = np.array(ts_list, dtype=np.int64)
    currents   = np.array(i_list,  dtype=np.float64)

    # Build (n × 8) uint8 matrix; fill X from previous valid state
    n = len(din_list)
    din_matrix = np.zeros((n, 8), dtype=np.uint8)
    prev = np.zeros(8, dtype=np.uint8)
    for idx, s in enumerate(din_list):
        for ch in range(min(8, len(s))):
            b = s[ch]
            if b == "1":
                prev[ch] = 1
            elif b == "0":
                prev[ch] = 0
            # 'X' → keep prev unchanged
        din_matrix[idx] = prev

    return timestamps, currents, din_matrix


def _decode_din_ppk2(dig_u16: np.ndarray) -> np.ndarray:
    """
    Decode the uint16 DIN word from a PPK2 session.raw into an (n × 8) uint8 matrix.

    Each channel uses 2 bits (see _DIN_BIT_SHIFTS):
        0b01 = LOW, 0b10 = HIGH, 0b11 = X (metastable → forward-filled from last valid).
    """
    n = len(dig_u16)
    din = np.zeros((n, 8), dtype=np.uint8)
    for ch, shift in enumerate(_DIN_BIT_SHIFTS):
        pair = (dig_u16 >> shift) & 0x03          # 1=LOW, 2=HIGH, 3=X
        valid_mask = pair != 3                     # False where metastable
        vals = (pair == 2).astype(np.uint8)        # HIGH → 1, LOW/X → 0

        # Forward-fill X: propagate last valid index, then index into vals
        idx = np.arange(n, dtype=np.int64)
        valid_idx = np.where(valid_mask, idx, np.int64(0))
        np.maximum.accumulate(valid_idx, out=valid_idx)
        din[:, ch] = vals[valid_idx]
    return din


def load_ppk2(path: Path):
    """
    Read a PPK2 native .ppk2 file (ZIP archive with session.raw + metadata.json).

    Returns:
        timestamps  – float64 array, milliseconds from recording start (t[0] = 0.0)
        currents    – float64 array, microamperes
        din_matrix  – uint8 array (n × 8), one column per digital channel D0..D7
    """
    with zipfile.ZipFile(path) as z:
        meta = json.loads(z.read("metadata.json"))
        raw  = z.read("session.raw")

    sps = int(meta["metadata"]["samplesPerSecond"])
    n   = len(raw) // 6  # stride = 4 (float32) + 2 (uint16)

    arr      = np.frombuffer(raw[:n * 6], dtype=np.uint8).reshape(n, 6)
    currents = arr[:, :4].copy().view(np.float32).flatten().astype(np.float64)
    dig_u16  = arr[:, 4:6].copy().view(np.uint16).flatten()

    timestamps = np.arange(n, dtype=np.float64) * (1000.0 / sps)
    din_matrix = _decode_din_ppk2(dig_u16)

    return timestamps, currents, din_matrix


# ===========================================================================
# Signal processing
# ===========================================================================

def parse_din_channels(din_matrix):
    """Return list of 8 numpy arrays (uint8), one per channel."""
    return [din_matrix[:, ch] for ch in range(8)]


def find_high_intervals(timestamps: np.ndarray, channel: np.ndarray):
    """
    Find all contiguous HIGH (=1) runs in *channel* using numpy edge detection.
    Returns list of (t_start_ms, t_end_ms, idx_start, idx_end_exclusive).
    """
    # Pad with 0 on both sides so rising/falling edges at boundaries are captured
    padded = np.concatenate(([0], channel, [0])).astype(np.int8)
    diff   = np.diff(padded)          # +1 = rising, -1 = falling
    rising  = np.where(diff ==  1)[0]  # indices into original array
    falling = np.where(diff == -1)[0]

    intervals = []
    for r, f in zip(rising, falling):
        intervals.append((int(timestamps[r]), int(timestamps[f - 1]), int(r), int(f)))
    return intervals


def detect_boot_end_idx(channels):
    """
    Boot phase = t[0] … first falling edge (1→0) on any digital channel.
    Returns the sample index just after the first pulse has ended, or None.
    """
    first_fall = None
    for ch_data in channels:
        padded = np.concatenate(([0], ch_data, [0])).astype(np.int8)
        falling = np.where(np.diff(padded) == -1)[0]
        if len(falling) > 0:
            f = int(falling[0])
            if first_fall is None or f < first_fall:
                first_fall = f
    return first_fall


# ===========================================================================
# Statistics
# ===========================================================================

def _energy_uAh(c: np.ndarray, t: np.ndarray) -> float:
    """
    Trapezoidal integration: µA × ms → µAh  (÷ 3 600 000).
    """
    return float(np.trapezoid(c, t)) / 3_600_000.0


def interval_stats(currents: np.ndarray, timestamps: np.ndarray,
                   idx_start: int, idx_end: int):
    """Statistics for samples[idx_start : idx_end] using numpy."""
    c = currents[idx_start:idx_end]
    t = timestamps[idx_start:idx_end]
    n = len(c)
    if n == 0:
        return 0.0, 0.0, 0.0, 0
    avg    = float(np.mean(c))
    peak   = float(np.max(c))
    energy = _energy_uAh(c, t) if n > 1 else 0.0
    return avg, peak, energy, n


def overall_stats(currents: np.ndarray, timestamps: np.ndarray):
    """Full-slice summary: avg, peak, energy — all via numpy."""
    n = len(currents)
    if n == 0:
        return 0.0, 0.0, 0.0
    avg    = float(np.mean(currents))
    peak   = float(np.max(currents))
    energy = _energy_uAh(currents, timestamps) if n > 1 else 0.0
    return avg, peak, energy


# ===========================================================================
# Formatting helpers
# ===========================================================================

def sep(char="─", width=COL_W):
    print(char * width)


def hr_duration(hours):
    """Convert decimal hours to a human-readable string."""
    if hours >= 24 * 365:
        return f"{hours / (24 * 365):.1f} years"
    if hours >= 24:
        return f"{hours / 24:.1f} days  ({hours:.0f} h)"
    return f"{hours:.1f} h"


def battery_table(avg_uA):
    """Return formatted battery life table as a string."""
    lines = []
    lines.append(f"\n  {'Battery':28s} {'Capacity':>10s}   {'Estimated runtime':>22s}")
    lines.append("  " + "─" * 66)
    for label, cap_mAh, note in BATTERIES:
        if avg_uA <= 0:
            runtime_str = "∞"
        else:
            hours = (cap_mAh * 1_000) / avg_uA   # µAh / µA = h
            runtime_str = hr_duration(hours)
        lines.append(
            f"  {label:28s} {cap_mAh:>7d} mAh   {runtime_str:>22s}  ({note})"
        )
    return "\n".join(lines)


# ===========================================================================
# Per-second profile
# ===========================================================================

def per_second_profile(timestamps: np.ndarray, currents: np.ndarray,
                       channels: list, active_chs: list):
    """
    Print a table with one row per second using numpy binning.
    np.searchsorted makes bucketing O(n log n) and avoids Python loops.
    """
    sep()
    print("PER-SECOND PROFILE")
    sep()

    t0 = int(timestamps[0])
    rel_ms = timestamps - t0
    seconds = (rel_ms // 1000).astype(np.int64)
    max_sec = int(seconds[-1])

    din_hdr = "".join(f"  D{c}" for c in active_chs)
    hdr = f"  {'sec':>5}  {'avg µA':>9}  {'min µA':>9}  {'max µA':>9}  {'n':>5}{din_hdr}"
    print(hdr)
    print("  " + "─" * (len(hdr) - 2))

    # Pre-stack DIN channels for active ones
    din_arr = np.stack([channels[c] for c in active_chs], axis=1) if active_chs else None

    for s in range(max_sec + 1):
        mask = seconds == s
        if not np.any(mask):
            continue
        c_sec = currents[mask]
        cnt   = len(c_sec)
        avg   = float(np.mean(c_sec))
        mn    = float(np.min(c_sec))
        mx    = float(np.max(c_sec))
        din_cols = ""
        if din_arr is not None:
            for ci, ch in enumerate(active_chs):
                ratio = float(np.mean(din_arr[mask, ci]))
                din_cols += f"  {'H' if ratio > 0.5 else 'L':>3}"
        print(f"  {s:>5}  {avg:>9.1f}  {mn:>9.1f}  {mx:>9.1f}  {cnt:>5}{din_cols}")


# ===========================================================================
# Main analysis
# ===========================================================================

def analyse(args):
    path = Path(args.csv)
    if not path.exists():
        sys.exit(f"ERROR: File not found: {path}")

    print(f"\nPPK2 Power Analyser  ▸  {path.name}")
    sep("═")

    # ── Load ─────────────────────────────────────────────────────────────────
    print("Loading …", end=" ", flush=True)
    if path.suffix.lower() == ".ppk2":
        timestamps, currents, din_matrix = load_ppk2(path)
        fmt = "PPK2 binary (.ppk2)"
    else:
        timestamps, currents, din_matrix = load_csv(path)
        fmt = "CSV (.csv)"
    n = len(timestamps)
    if n < 2:
        sys.exit("ERROR: CSV contains fewer than 2 valid rows.")

    print(f"done  ({n:,} rows)")
    t_total_ms = float(timestamps[-1] - timestamps[0])
    dt_avg_ms  = t_total_ms / (n - 1)
    fs_approx  = 1000.0 / dt_avg_ms if dt_avg_ms > 0 else 0

    sep()
    print("RECORDING INFO")
    sep()
    print(f"  Format    : {fmt}")
    print(f"  Samples   : {n:,}")
    print(f"  Duration  : {t_total_ms / 1000:.3f} s  "
          f"({t_total_ms / 60_000:.1f} min)")
    print(f"  Avg rate  : {fs_approx:.0f} S/s  (mean Δt = {dt_avg_ms:.3f} ms)")

    channels   = parse_din_channels(din_matrix)
    active_chs = [ch for ch in range(8) if np.any(channels[ch] == 1)]
    print(f"  Active DINs: {', '.join(f'D{c}' for c in active_chs) if active_chs else 'none'}")

    # ── Boot detection ───────────────────────────────────────────────────────
    boot_end_idx = None
    if active_chs and not args.no_boot_exclusion:
        boot_end_idx = detect_boot_end_idx(channels)

    if boot_end_idx is not None:
        boot_end_ms = int(timestamps[boot_end_idx])
        print(f"  Boot phase : 0 – {boot_end_ms} ms  (excluded from battery estimates)")
        ss_ts = timestamps[boot_end_idx:]
        ss_i  = currents[boot_end_idx:]
    else:
        if not active_chs:
            print("  Boot phase : not detected (no DIN activity)")
        elif args.no_boot_exclusion:
            print("  Boot phase : exclusion disabled (--no-boot-exclusion)")
        boot_end_idx = 0
        ss_ts = timestamps
        ss_i  = currents

    # ── Overall stats ────────────────────────────────────────────────────────
    sep()
    print("FULL RECORDING — Current Statistics")
    sep()
    full_avg, full_peak, full_energy = overall_stats(currents, timestamps)
    print(f"  Average : {full_avg:>10.2f} µA")
    print(f"  Peak    : {full_peak:>10.2f} µA  ({full_peak / 1000:.3f} mA)")
    print(f"  Energy  : {full_energy:>10.4f} µAh  "
          f"({full_energy * 3.3e-3:.6f} mWh @ 3.3 V)")

    # ── Steady-state stats ───────────────────────────────────────────────────
    if boot_end_idx > 0:
        sep()
        print("STEADY STATE — Current Statistics  (boot phase excluded)")
        sep()
        ss_avg, ss_peak, ss_energy = overall_stats(ss_i, ss_ts)
        ss_dur_s = (int(ss_ts[-1]) - int(ss_ts[0])) / 1000.0
        print(f"  Duration : {ss_dur_s:.3f} s  ({len(ss_i):,} samples)")
        print(f"  Average  : {ss_avg:>10.2f} µA")
        print(f"  Peak     : {ss_peak:>10.2f} µA  ({ss_peak / 1000:.3f} mA)")
        print(f"  Energy   : {ss_energy:>10.4f} µAh  "
              f"({ss_energy * 3.3e-3:.6f} mWh @ 3.3 V)")
    else:
        ss_avg = full_avg

    # ── Per-channel HIGH-phase analysis ──────────────────────────────────────
    for ch in active_chs:
        intervals = find_high_intervals(timestamps, channels[ch])
        if not intervals:
            continue

        sep()
        print(f"D{ch} HIGH-PHASE ANALYSIS  ({len(intervals)} pulse(s))")
        sep()

        hdr = (f"  {'#':>3}  {'t_start ms':>11}  {'t_end ms':>10}  "
               f"{'dur ms':>8}  {'avg µA':>8}  {'peak µA':>9}  {'energy µAh':>12}")
        print(hdr)
        print("  " + "─" * (len(hdr) - 2))

        total_dur_weighted_avg = 0.0
        total_dur = 0
        total_energy = 0.0
        peak_all = 0.0

        for k, (ts_s, ts_e, i0, i1) in enumerate(intervals):
            avg, peak, energy, samp = interval_stats(currents, timestamps, i0, i1)
            dur = ts_e - ts_s
            total_dur_weighted_avg += avg * dur
            total_dur += dur
            total_energy += energy
            if peak > peak_all:
                peak_all = peak
            print(f"  {k + 1:>3}  {ts_s:>11,}  {ts_e:>10,}  "
                  f"{dur:>8,}  {avg:>8.1f}  {peak:>9.1f}  {energy:>12.6f}")

        # Summary row
        comb_avg = total_dur_weighted_avg / total_dur if total_dur > 0 else 0.0
        print("  " + "─" * (len(hdr) - 2))
        print(f"  {'Σ':>3}  {'':>11}  {'':>10}  "
              f"{total_dur:>8,}  {comb_avg:>8.1f}  {peak_all:>9.1f}  {total_energy:>12.6f}")
        print(f"\n  Weighted average during D{ch}=HIGH: {comb_avg:.1f} µA")
        print(f"  Total HIGH time            : {total_dur:,} ms  "
              f"({total_dur / 1000:.3f} s)")
        print(f"  Total energy during HIGH   : {total_energy:.6f} µAh  "
              f"= {total_energy * 3.3e-3:.6f} mWh")

    # ── Per-second profile ───────────────────────────────────────────────────
    if args.per_second:
        per_second_profile(timestamps, currents, channels, active_chs)

    # ── Battery life estimate ────────────────────────────────────────────────
    sep()
    print("BATTERY LIFE ESTIMATE")
    sep()
    print(f"  Based on steady-state average: {ss_avg:.2f} µA")
    print(f"  (boot phase {'excluded' if boot_end_idx > 0 else 'not detected / included'})")
    print(battery_table(ss_avg))
    print()
    sep()


# ===========================================================================
# Entry point
# ===========================================================================

def main():
    parser = argparse.ArgumentParser(
        description="Nordic PPK2 Power Analyser — supports .ppk2 (binary) and .csv (text export), up to 8 digital inputs",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  uv run scripts/ppk_analysis.py data/recording.ppk2
  uv run scripts/ppk_analysis.py data/recording.csv
  uv run scripts/ppk_analysis.py data/recording.ppk2 --per-second
  uv run scripts/ppk_analysis.py data/recording.csv  --no-boot-exclusion
""",
    )
    parser.add_argument("input", help="Path to PPK2 recording (.ppk2 binary or .csv text export)")
    parser.add_argument(
        "--per-second", "-s",
        action="store_true",
        help="Print per-second current profile table",
    )
    parser.add_argument(
        "--no-boot-exclusion",
        action="store_true",
        help="Include boot phase in battery life estimate",
    )
    args = parser.parse_args()
    args.csv = args.input  # internal alias used by analyse()
    analyse(args)


if __name__ == "__main__":
    main()
