#!/usr/bin/env python3
"""
Extract x(t) and y(t) trajectories from the K04 §4.2 bouncing-ball simulation.

Runs qss1-ball-downstairs with NDJSON logging and filters sim_state events for
int_x and int_y as they stream in — no log file is written to disk.

Output (written to figures/):
    traj_x.dat   space-separated (t, q) for horizontal position
    traj_y.dat   space-separated (t, q) for vertical position (downsampled)

These files are read directly by pgfplots in results.tex.

Usage:
    python3 extract_trajectories.py [path/to/qss1-ball-downstairs]

Downsampling:
    The y trajectory can have O(10^4) events due to spring-damper contact
    oscillations.  Points are thinned to at most one per DT_MIN seconds so
    pgfplots compiles in reasonable time, while preserving all visible detail
    at the scale of the printed figure.
"""

import json
import os
import subprocess
import sys

DT_MIN = 0.002  # minimum time gap between kept y samples (s)

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
        "Build the project first (cmake --build), or pass the path as argument."
    )

# ── Stream simulation ─────────────────────────────────────────────────────────
print(f"Running: {_binary}", flush=True)

rows_x = []          # (t, q) — all x events (typically O(10^3), kept in full)
rows_y = []          # (t, q) — downsampled y events
last_t_y = -1.0      # last kept y sample time

proc = subprocess.Popen(
    [_binary], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    text=True, bufsize=1,
)
for raw in proc.stdout:
    raw = raw.strip()
    if not raw:
        continue
    try:
        ev = json.loads(raw)
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
        rows_x.append((t, q))
    elif t - last_t_y >= DT_MIN:
        rows_y.append((t, q))
        last_t_y = t

proc.wait()
print(
    f"Done.  x: {len(rows_x):,} pts   y: {len(rows_y):,} pts "
    f"(from {len(rows_y)} kept, DT_MIN={DT_MIN}s)",
    flush=True,
)

# ── Write .dat files ──────────────────────────────────────────────────────────
_fig_dir = os.path.join(_script_dir, "figures")
os.makedirs(_fig_dir, exist_ok=True)

for name, rows in [("traj_x.dat", rows_x), ("traj_y.dat", rows_y)]:
    path = os.path.join(_fig_dir, name)
    with open(path, "w") as f:
        f.write("t q\n")
        for t, q in rows:
            f.write(f"{t:.6f} {q:.6f}\n")
    print(f"Saved: {path}  ({len(rows):,} rows)", flush=True)
