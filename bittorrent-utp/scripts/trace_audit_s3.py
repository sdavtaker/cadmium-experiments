#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Trace audit for the stage-3 deterministic two-client BitTorrent/uTP scenario.

Runs bt-utp-s3-det (built alongside bt-utp-tests) via subprocess and checks
that every BEP3 exchange type this scenario's own design can actually
produce appears at least once in the filtered NDJSON trace on stdout.

CANCEL, KEEPALIVE, and a bare CHOKE are deliberately excluded from the
required list: this scenario has no packet loss (bottleneck_channel here
is lossless/unbounded), no idle-timeout mechanism, and both clients use
the stub_always_unchoke policy (which by construction never sends CHOKE,
only UNCHOKE) -- none of those three can ever be produced by this specific
scenario as currently built. A future stage (a real choking policy, or a
lossy_channel variant of this scenario) would need its own audit list.
"""
import argparse
import re
import subprocess
import sys

# Exact wire-format text each BEP3 message's operator<< produces (bep3.hpp),
# scoped where needed to avoid a false match against a related message
# (REQUEST/PIECE/HAVE carry an "idx:" field no other message text has;
# INTERESTED needs its own check since NOT_INTERESTED contains it as a
# substring -- handled separately below, not listed here).
REQUIRED_SUBSTRINGS = [
    "HANDSHAKE",
    "BITFIELD",
    "REQUEST idx:",
    "PIECE idx:",
    "HAVE idx:",
    "UNCHOKE",
    "NOT_INTERESTED",
]

BARE_INTERESTED_RE = re.compile(r"(?<!NOT_)INTERESTED")


def run_driver(binary: str, t_max: float) -> str:
    result = subprocess.run(
        [binary, str(t_max)], capture_output=True, text=True, timeout=300, check=True
    )
    return result.stdout


def audit(trace: str) -> dict[str, int]:
    counts = {event: trace.count(event) for event in REQUIRED_SUBSTRINGS}
    counts["INTERESTED (bare)"] = len(BARE_INTERESTED_RE.findall(trace))
    return counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        default="build/bittorrent-utp/bt-utp-s3-det",
        help="path to the bt-utp-s3-det executable",
    )
    parser.add_argument(
        "--t-max", type=float, default=200.0, help="simulated seconds to run before giving up"
    )
    args = parser.parse_args()

    trace = run_driver(args.binary, args.t_max)
    counts = audit(trace)

    for event, count in counts.items():
        print(f"{event}: {count}")

    missing = [event for event, count in counts.items() if count == 0]
    if missing:
        print(f"MISSING: {', '.join(missing)}", file=sys.stderr)
        return 1

    print("OK: every required BEP3 exchange type is present in the trace")
    return 0


if __name__ == "__main__":
    sys.exit(main())
