#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Propagation-time analyzer for bt-utp-* NDJSON traces (bead zgmx).

Reads a filtered NDJSON trace produced by one of the bt-utp-{s3,s4,s4-anti-snub}-det
executables (see run_experiment.sh) and computes:

  - propagation time: sim-time from t=0 content availability at the seed to
    the receiver's last piece-completion event (the sim_time of the receiver
    wire model's Nth distinct HAVE announcement, where N = --total-pieces).
    This is the same definition test_s4_two_client_det.cpp's
    track_have_progress() already uses for its golden-value bounds.
  - overhead counts: occurrences of each BEP3 message type across the trace.
  - cwnd time series per socket, and an observed-throughput ("rate") time
    series binned over --rate-bin-seconds, both from piece completions.

cwnd data is only present if the trace was captured with --full-trace (see
main_s4_det.cpp's usage comment) — sim_state is off by default because it
multiplies trace volume ~15x. Without it, the cwnd figure/CSV are skipped
with a note, everything else (propagation time, overhead counts, rate) still
works off the default filtered trace.

Usage:
  analyze_propagation.py --log run.ndjson --label s4-det --total-pieces 40 \
      --piece-bytes 262144 --receiver client_b.wire --socket client_a.socket \
      --socket client_b.socket --out-dir docs
"""
import argparse
import csv
import json
import re
import sys
from pathlib import Path

# Exact wire-format text each BEP3 message's operator<< produces (bep3.hpp),
# scoped where needed to avoid a false match against a related message —
# same convention as trace_audit_s3.py.
MESSAGE_SUBSTRINGS = [
    "HANDSHAKE",
    "BITFIELD",
    "REQUEST idx:",
    "PIECE idx:",
    "HAVE idx:",
    "NOT_INTERESTED",
    "CANCEL",
    "KEEPALIVE",
]
# UNCHOKE/CHOKE and INTERESTED/NOT_INTERESTED share a suffix/prefix, so bare
# forms need a negative lookaround rather than a plain substring count.
BARE_CHOKE_RE = re.compile(r"(?<!UN)CHOKE")
UNCHOKE_RE = re.compile(r"UNCHOKE")
BARE_INTERESTED_RE = re.compile(r"(?<!NOT_)INTERESTED")

HAVE_IDX_RE = re.compile(r"HAVE idx:(\d+)")
CWND_RE = re.compile(r"cwnd:(\d+)")


def die(msg: str):
    sys.exit(f"error: {msg}")


def iter_records(path: Path):
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def split_model_msg(msg: str) -> tuple[str, str]:
    """sim_messages_collect/sim_state msg fields are '<model_id> <rest>'."""
    model, _, rest = msg.partition(" ")
    return model, rest


def message_counts(records: list[dict]) -> dict[str, int]:
    text = "\n".join(
        r["msg"] for r in records if r.get("event") == "sim_messages_collect" and "msg" in r
    )
    counts = {s: text.count(s) for s in MESSAGE_SUBSTRINGS}
    counts["UNCHOKE"] = len(UNCHOKE_RE.findall(text))
    # BARE_CHOKE_RE's negative lookbehind already excludes the "CHOKE" that's
    # part of "UNCHOKE", so no further adjustment against the UNCHOKE count
    # is needed here.
    counts["CHOKE (bare)"] = len(BARE_CHOKE_RE.findall(text))
    counts["INTERESTED (bare)"] = len(BARE_INTERESTED_RE.findall(text))
    return counts


def receiver_have_progress(records: list[dict], receiver_prefix: str, total_pieces: int):
    """Distinct piece indices announced by receiver_prefix's HAVE messages, and
    the sim_time each successive distinct index first appears (index i in the
    returned list is the sim_time the (i+1)-th distinct piece was completed).
    """
    seen: set[int] = set()
    completions: list[tuple[float, int]] = []  # (sim_time, cumulative distinct count)
    for r in records:
        if r.get("event") != "sim_messages_collect" or "msg" not in r:
            continue
        model, rest = split_model_msg(r["msg"])
        if model != receiver_prefix:
            continue
        sim_time = r.get("sim_time")
        for idx_str in HAVE_IDX_RE.findall(rest):
            idx = int(idx_str)
            if idx not in seen:
                seen.add(idx)
                if sim_time is not None:
                    completions.append((float(sim_time), len(seen)))
        if len(seen) >= total_pieces:
            break
    return seen, completions


def cwnd_series(records: list[dict], socket_names: list[str]) -> dict[str, list[tuple[float, int]]]:
    series: dict[str, list[tuple[float, int]]] = {name: [] for name in socket_names}
    for r in records:
        if r.get("event") != "sim_state" or "msg" not in r:
            continue
        model, rest = split_model_msg(r["msg"])
        if model not in series:
            continue
        sim_time = r.get("sim_time")
        m = CWND_RE.search(rest)
        if sim_time is not None and m:
            series[model].append((float(sim_time), int(m.group(1))))
    return series


def rate_series(completions: list[tuple[float, int]], piece_bytes: int, bin_seconds: float):
    """Bytes delivered per bin_seconds-wide window, from piece-completion times."""
    if not completions:
        return []
    max_t = completions[-1][0]
    n_bins = max(1, int(max_t // bin_seconds) + 1)
    bytes_per_bin = [0] * n_bins
    prev_count = 0
    for sim_time, cum_count in completions:
        delivered = cum_count - prev_count
        prev_count = cum_count
        b = min(int(sim_time // bin_seconds), n_bins - 1)
        bytes_per_bin[b] += delivered * piece_bytes
    return [
        (i * bin_seconds, bytes_per_bin[i] / bin_seconds) for i in range(n_bins)
    ]


def write_csv(path: Path, header: list[str], rows: list[tuple]):
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        w.writerows(rows)


def write_svg_lines(path: Path, series: dict[str, list[tuple[float, float]]], title: str,
                     x_label: str, y_label: str):
    """Minimal stdlib-only SVG line chart — no external plotting dependency,
    consistent with this repo's existing scripts (all stdlib-only so far).
    """
    width, height = 900, 500
    margin = {"left": 70, "right": 20, "top": 40, "bottom": 60}
    plot_w = width - margin["left"] - margin["right"]
    plot_h = height - margin["top"] - margin["bottom"]

    all_points = [p for pts in series.values() for p in pts]
    if not all_points:
        path.write_text(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}">'
            f'<text x="20" y="30">{title}: no data</text></svg>'
        )
        return

    xs = [p[0] for p in all_points]
    ys = [p[1] for p in all_points]
    x_min, x_max = min(xs), max(xs)
    y_min, y_max = 0.0, max(ys) * 1.05 if max(ys) > 0 else 1.0
    x_span = (x_max - x_min) or 1.0
    y_span = (y_max - y_min) or 1.0

    def sx(x):
        return margin["left"] + (x - x_min) / x_span * plot_w

    def sy(y):
        return margin["top"] + plot_h - (y - y_min) / y_span * plot_h

    colors = ["#2563eb", "#dc2626", "#059669", "#d97706", "#7c3aed"]
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'font-family="sans-serif" font-size="12">',
        f'<rect width="{width}" height="{height}" fill="white"/>',
        f'<text x="{width / 2}" y="20" text-anchor="middle" font-size="14">{title}</text>',
        f'<line x1="{margin["left"]}" y1="{margin["top"]}" x2="{margin["left"]}" '
        f'y2="{margin["top"] + plot_h}" stroke="black"/>',
        f'<line x1="{margin["left"]}" y1="{margin["top"] + plot_h}" '
        f'x2="{margin["left"] + plot_w}" y2="{margin["top"] + plot_h}" stroke="black"/>',
        f'<text x="{margin["left"] + plot_w / 2}" y="{height - 15}" text-anchor="middle">'
        f'{x_label}</text>',
        f'<text x="15" y="{margin["top"] + plot_h / 2}" text-anchor="middle" '
        f'transform="rotate(-90 15 {margin["top"] + plot_h / 2})">{y_label}</text>',
    ]
    for (name, pts), color in zip(series.items(), colors * (len(series) // len(colors) + 1)):
        if not pts:
            continue
        points_str = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y in pts)
        parts.append(f'<polyline points="{points_str}" fill="none" stroke="{color}" '
                     f'stroke-width="2"/>')
    legend_y = margin["top"]
    for (name, _), color in zip(series.items(), colors * (len(series) // len(colors) + 1)):
        parts.append(f'<rect x="{width - 160}" y="{legend_y}" width="10" height="10" '
                     f'fill="{color}"/>')
        parts.append(f'<text x="{width - 145}" y="{legend_y + 9}">{name}</text>')
        legend_y += 16
    parts.append("</svg>")
    path.write_text("\n".join(parts))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--log", required=True, type=Path, help="path to the NDJSON trace")
    parser.add_argument("--label", required=True, help="short name for output files, e.g. s4-det")
    parser.add_argument("--total-pieces", type=int, required=True)
    parser.add_argument("--piece-bytes", type=int, required=True)
    parser.add_argument("--receiver", required=True,
                        help="model id of the receiving side's peer_wire, e.g. client_b.wire")
    parser.add_argument("--socket", action="append", default=[],
                        help="model id of a utp_socket to extract cwnd for (repeatable)")
    parser.add_argument("--rate-bin-seconds", type=float, default=1.0)
    parser.add_argument("--out-dir", type=Path, default=Path("."))
    args = parser.parse_args()

    if not args.log.is_file():
        die(f"log file not found: {args.log}")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    records = list(iter_records(args.log))
    if not records:
        die(f"{args.log} contains no parseable NDJSON records")

    counts = message_counts(records)
    seen, completions = receiver_have_progress(records, args.receiver, args.total_pieces)
    propagation_time = completions[-1][0] if len(seen) >= args.total_pieces and completions else None
    cwnds = cwnd_series(records, args.socket) if args.socket else {}
    rates = rate_series(completions, args.piece_bytes, args.rate_bin_seconds)

    results = {
        "label": args.label,
        "log": str(args.log),
        "total_pieces": args.total_pieces,
        "pieces_completed": len(seen),
        "propagation_time_s": propagation_time,
        "message_counts": counts,
        "total_messages": sum(counts.values()),
    }

    print(f"=== {args.label} ===")
    print(f"pieces completed: {len(seen)}/{args.total_pieces}")
    if propagation_time is not None:
        print(f"propagation time: {propagation_time:.4f}s")
    else:
        print("propagation time: INCOMPLETE (not all pieces delivered within the trace)")
    print("message counts:")
    for k, v in counts.items():
        print(f"  {k:>20}: {v}")
    print(f"total messages: {results['total_messages']}")

    results_path = args.out_dir / f"{args.label}_results.json"
    results_path.write_text(json.dumps(results, indent=2) + "\n")
    print(f"wrote {results_path}")

    if rates:
        rate_csv = args.out_dir / f"{args.label}_rate.csv"
        write_csv(rate_csv, ["bin_start_s", "rate_Bps"], rates)
        write_svg_lines(rate_csv.with_suffix(".svg"), {"delivery rate": rates},
                        f"{args.label}: delivery rate", "sim time (s)", "bytes/s")
        print(f"wrote {rate_csv} and {rate_csv.with_suffix('.svg')}")

    if args.socket:
        if any(cwnds.values()):
            cwnd_csv = args.out_dir / f"{args.label}_cwnd.csv"
            with cwnd_csv.open("w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["socket", "sim_time_s", "cwnd_bytes"])
                for name, pts in cwnds.items():
                    for t, c in pts:
                        w.writerow([name, t, c])
            write_svg_lines(cwnd_csv.with_suffix(".svg"), cwnds, f"{args.label}: cwnd",
                            "sim time (s)", "cwnd (bytes)")
            print(f"wrote {cwnd_csv} and {cwnd_csv.with_suffix('.svg')}")
        else:
            print("no sim_state data found for --socket names given — "
                 "re-run the driver with --full-trace to get cwnd data", file=sys.stderr)

    return 0 if propagation_time is not None else 1


if __name__ == "__main__":
    sys.exit(main())
