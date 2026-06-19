#!/usr/bin/env python3
"""
Generate trajectory plots for the K04 §4.2 bouncing-ball-downstairs experiment.

Runs the qss1-ball-downstairs binary with NDJSON logging enabled and processes
sim_state events for int_x and int_y as they stream in — no log file is written.

Usage:
    python3 plot_results.py [path/to/qss1-ball-downstairs]

Output:
    figures/ball_trajectory.pdf
"""

import json
import math
import os
import subprocess
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ── Locate binary ─────────────────────────────────────────────────────────────
_script_dir = os.path.dirname(os.path.abspath(__file__))
if len(sys.argv) > 1:
    _binary = sys.argv[1]
else:
    _binary = os.path.join(
        _script_dir, "..", "..", "build",
        "qss1-ball-downstairs", "qss1-ball-downstairs",
    )
_binary = os.path.realpath(_binary)
if not os.path.isfile(_binary):
    sys.exit(
        f"Binary not found: {_binary}\n"
        "Build the project first (cmake --build), or pass the path as an argument."
    )

# ── Stream simulation and collect trajectories ────────────────────────────────
print(f"Running: {_binary}", flush=True)

tx, qx = [], []  # time and q for int_x (horizontal position)
ty, qy = [], []  # time and q for int_y (vertical position)

proc = subprocess.Popen(
    [_binary], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    text=True, bufsize=1,
)
for raw in proc.stdout:
    line = raw.strip()
    if not line:
        continue
    try:
        ev = json.loads(line)
    except json.JSONDecodeError:
        continue
    if ev.get("event") != "sim_state":
        continue
    msg = ev.get("msg", "")
    t = ev.get("sim_time")
    if t is None:
        continue

    is_x = msg.startswith("int_x ")
    is_y = msg.startswith("int_y ")
    if not (is_x or is_y):
        continue

    # msg: "int_x x=… u=… q=<val> dq=… sigma=…"
    try:
        q = float(msg.split(" q=")[1].split()[0])
    except (IndexError, ValueError):
        continue

    if is_x:
        tx.append(t)
        qx.append(q)
    else:
        ty.append(t)
        qy.append(q)

proc.wait()
print(f"Done.  x events: {len(tx):,}   y events: {len(ty):,}", flush=True)

if not tx or not ty:
    sys.exit("No trajectory data captured — check that the binary produces NDJSON output.")

# ── Staircase reference lines for y panel ─────────────────────────────────────
# Steps are 1 m wide × 1 m tall; first step height = H = 10.
# The floor height below position x is floor(H + 1 - x).
H = 10.0
x_min, x_max = min(qx), max(qx)
stair_levels = sorted(
    {math.floor(H + 1.0 - x) for x in qx},
    reverse=True,
)

# ── Plot ──────────────────────────────────────────────────────────────────────
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(7.0, 6.0), sharex=True)
fig.subplots_adjust(hspace=0.06)

# — x(t): horizontal position ——————————————————————————————————————————————————
ax1.plot(tx, qx, color="steelblue", linewidth=0.7, label=r"$x(t)$")
ax1.set_ylabel(r"$x$ (m)", fontsize=11)
ax1.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax1.grid(True, which="major", alpha=0.25)
ax1.legend(loc="upper left", fontsize=10)

# — y(t): vertical position with stair-edge reference ——————————————————————————
ax2.plot(ty, qy, color="steelblue", linewidth=0.7, label=r"$y(t)$")
for level in stair_levels:
    ax2.axhline(
        y=level, color="grey", linewidth=0.5, linestyle="--", alpha=0.45,
        label=f"stair $y={level}$" if level == stair_levels[0] else None,
    )
ax2.set_ylabel(r"$y$ (m)", fontsize=11)
ax2.set_xlabel(r"$t$ (s)", fontsize=11)
ax2.yaxis.set_minor_locator(ticker.AutoMinorLocator())
ax2.grid(True, which="major", alpha=0.25)
ax2.legend(loc="upper right", fontsize=10)

ax1.set_title(
    r"K04 §4.2 — QSS1 Bouncing Ball Downstairs ($t \in [0, 10]$ s)",
    fontsize=11, pad=6,
)

# ── Save ──────────────────────────────────────────────────────────────────────
_fig_dir = os.path.join(_script_dir, "figures")
os.makedirs(_fig_dir, exist_ok=True)
_out = os.path.join(_fig_dir, "ball_trajectory.pdf")
fig.savefig(_out, bbox_inches="tight")
print(f"Saved: {_out}", flush=True)
