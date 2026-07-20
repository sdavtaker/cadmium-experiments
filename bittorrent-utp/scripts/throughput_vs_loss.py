#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Stage-2 sto-pass statistical check: mean throughput must be monotonically
non-increasing as the channel's drop probability p increases (fable_plan.md
section 2, sto pass point 7). This is a trend across many seeds, not a
single-run assertion a ctest can express directly, so it drives the built
bt-utp-s2-sto executable across a p sweep with >=10 seeds each and checks
the resulting per-p mean throughput sequence.

A small tolerance band absorbs finite-sample noise between adjacent p
values without accepting a real reversal of the trend.

Usage: throughput_vs_loss.py <path-to-bt-utp-s2-sto-executable>
"""
import subprocess
import sys

P_SWEEP = [0.001, 0.005, 0.01, 0.02, 0.05]
SEEDS = range(1, 13)  # 12 seeds per p (>= 10 required)
TOLERANCE = 0.05  # allow a mean to be up to 5% above the previous p's mean


def die(msg: str):
    sys.exit(f"error: {msg}")


def run_one(exe: str, seed: int, p: float) -> dict:
    proc = subprocess.run([exe, str(seed), str(p)], capture_output=True, text=True, timeout=60)
    if proc.returncode not in (0, 1):
        die(f"{exe} {seed} {p} crashed: {proc.stderr.strip()}")
    fields = dict(kv.split("=", 1) for kv in proc.stdout.split())
    fields["delivered"] = int(fields["delivered"])
    fields["throughput_bps"] = float(fields["throughput_bps"])
    if not fields["delivered"]:
        die(f"seed={seed} p={p} did not deliver all bytes — reliability invariant violated")
    return fields


def main():
    if len(sys.argv) != 2:
        die("usage: throughput_vs_loss.py <path-to-bt-utp-s2-sto-executable>")
    exe = sys.argv[1]

    means = []
    print(f"{'p':>8}  {'mean_throughput_bps':>20}  {'seeds':>5}")
    for p in P_SWEEP:
        samples = [run_one(exe, seed, p)["throughput_bps"] for seed in SEEDS]
        mean = sum(samples) / len(samples)
        means.append(mean)
        print(f"{p:>8}  {mean:>20.1f}  {len(samples):>5}")

    violations = []
    for i in range(1, len(means)):
        if means[i] > means[i - 1] * (1.0 + TOLERANCE):
            violations.append((P_SWEEP[i - 1], means[i - 1], P_SWEEP[i], means[i]))

    if violations:
        print("\nMonotonicity violations (mean throughput rose more than "
              f"{TOLERANCE * 100:.0f}% from a lower p to a higher p):")
        for p_lo, m_lo, p_hi, m_hi in violations:
            print(f"  p={p_lo} (mean={m_lo:.1f}) -> p={p_hi} (mean={m_hi:.1f})")
        die("throughput is not monotonically non-increasing in loss probability")

    print("\nOK: mean throughput is monotonically non-increasing across the p sweep.")


if __name__ == "__main__":
    main()
