#!/usr/bin/env python3
"""Check the sampling quality of a CSV exported by the IMU BLE Logger dashboard.

Usage:
    python3 tools/check_sampling.py imu_log_2026-01-01_12-00.csv

Reports the number of samples, the effective sample rate, the spread of the
intervals between samples, and any gap larger than 1.5 nominal periods.
Only the Python standard library is required.
"""
import csv
import statistics
import sys
from collections import Counter

NOMINAL_HZ = 100.0


def main(path):
    with open(path, newline="", encoding="utf-8-sig") as f:
        rows = list(csv.DictReader(f))
    if len(rows) < 2:
        sys.exit("Not enough samples to analyze.")

    seq = [int(r["sequence"]) for r in rows]
    ts = [int(r["timestamp_us"]) for r in rows]
    dt = [b - a for a, b in zip(ts, ts[1:])]
    duration = (ts[-1] - ts[0]) / 1e6
    nominal_us = 1e6 / NOMINAL_HZ

    print(f"File               : {path}")
    print(f"Samples            : {len(rows)}")
    print(f"Sequence contiguous: {seq == list(range(len(seq)))}")
    print(f"Duration           : {duration:.3f} s")
    print(f"Effective rate     : {(len(rows) - 1) / duration:.2f} Hz (nominal {NOMINAL_HZ:.0f} Hz)")
    print(f"Interval (us)      : min {min(dt)}  median {statistics.median(dt):.0f}  "
          f"mean {statistics.mean(dt):.0f}  max {max(dt)}  stdev {statistics.pstdev(dt):.0f}")

    gaps = [(i, d) for i, d in enumerate(dt) if d > 1.5 * nominal_us]
    print(f"Gaps > 1.5 periods : {len(gaps)}")
    for i, d in gaps[:10]:
        print(f"  after sample {i}: {d / 1000:.1f} ms")

    print("Interval histogram (us: count):")
    for value, n in sorted(Counter(dt).items()):
        print(f"  {value}: {n}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
