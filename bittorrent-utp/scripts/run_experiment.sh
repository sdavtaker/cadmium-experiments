#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
#
# Hang-guard aware runner for a single bt-utp-*-det binary (bead zgmx): runs
# the binary with a wall-clock timeout, saves the raw NDJSON trace (untracked
# per repo convention -- *.ndjson is gitignored), and feeds it to
# analyze_propagation.py unless --no-analyze is given.
#
# Usage:
#   run_experiment.sh --binary PATH --label NAME --total-pieces N \
#       --piece-bytes N --receiver MODEL [options...] [-- extra analyzer args]
#
# Required:
#   --binary PATH        path to a bt-utp-*-det executable
#   --label NAME         short name for output files, e.g. s4-det
#   --total-pieces N     passed through to analyze_propagation.py
#   --piece-bytes N       passed through to analyze_propagation.py
#   --receiver MODEL     passed through to analyze_propagation.py
#
# Optional:
#   --t-max SECONDS       simulated seconds to run (default: binary's own default)
#   --socket MODEL        repeatable; forwarded to analyze_propagation.py.
#                         Passing any --socket implies --full-trace on the
#                         binary (cwnd data needs sim_state).
#   --full-trace          include sim_state even with no --socket given
#   --timeout SECONDS     wall-clock kill timeout for the binary (default:
#                         1800 -- NOT .hang_guard.json's generic 60s "binary"
#                         default, which is far too low for this specific
#                         scenario: measured on this Pi, bt-utp-s4-det with
#                         t_max=60 and no --full-trace already takes ~250s
#                         wall-clock and emits 361k trace lines; --full-trace
#                         (~15x volume per s3_two_client_det.hpp's own doc
#                         comment) can plausibly take an order of magnitude
#                         longer for the same t_max. Raise --timeout further
#                         for larger t_max or --full-trace runs.)
#   --out-dir DIR         where the raw log and analysis output land
#                         (default: results/)
#   --no-analyze          just run the binary and save the trace
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  run_experiment.sh --binary PATH --label NAME --total-pieces N \
      --piece-bytes N --receiver MODEL [options...] [-- extra analyzer args]

Required:
  --binary PATH        path to a bt-utp-*-det executable
  --label NAME         short name for output files, e.g. s4-det
  --total-pieces N     passed through to analyze_propagation.py
  --piece-bytes N      passed through to analyze_propagation.py
  --receiver MODEL     passed through to analyze_propagation.py

Optional:
  --t-max SECONDS      simulated seconds to run (default: binary's own default)
  --socket MODEL       repeatable; forwarded to analyze_propagation.py.
                       Passing any --socket implies --full-trace on the
                       binary (cwnd data needs sim_state).
  --full-trace         include sim_state even with no --socket given
  --timeout SECONDS    wall-clock kill timeout for the binary (default: 1800;
                       see the file-level comment for why this is much
                       higher than .hang_guard.json's generic 60s default)
  --out-dir DIR        where the raw log and analysis output land
                       (default: results/)
  --no-analyze         just run the binary and save the trace
EOF
}

BINARY=""
LABEL=""
TOTAL_PIECES=""
PIECE_BYTES=""
RECEIVER=""
T_MAX=""
TIMEOUT=1800
OUT_DIR="results"
NO_ANALYZE=0
FULL_TRACE=0
SOCKET_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --binary) BINARY="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    --total-pieces) TOTAL_PIECES="$2"; shift 2 ;;
    --piece-bytes) PIECE_BYTES="$2"; shift 2 ;;
    --receiver) RECEIVER="$2"; shift 2 ;;
    --t-max) T_MAX="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --socket) SOCKET_ARGS+=("--socket" "$2"); FULL_TRACE=1; shift 2 ;;
    --full-trace) FULL_TRACE=1; shift ;;
    --no-analyze) NO_ANALYZE=1; shift ;;
    --) shift; break ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 1 ;;
  esac
done

for required in BINARY LABEL; do
  if [[ -z "${!required}" ]]; then
    echo "error: --${required,,} is required" >&2
    exit 1
  fi
done
if [[ "$NO_ANALYZE" -eq 0 ]]; then
  for required in TOTAL_PIECES PIECE_BYTES RECEIVER; do
    if [[ -z "${!required}" ]]; then
      echo "error: --${required,,} is required unless --no-analyze is given" >&2
      exit 1
    fi
  done
fi
if [[ ! -x "$BINARY" ]]; then
  echo "error: not an executable file: $BINARY" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
LOG_PATH="$OUT_DIR/${LABEL}.ndjson"

BINARY_ARGS=()
[[ -n "$T_MAX" ]] && BINARY_ARGS+=("$T_MAX")
[[ "$FULL_TRACE" -eq 1 ]] && BINARY_ARGS+=("--full-trace")

echo "running: $BINARY ${BINARY_ARGS[*]:-} (timeout ${TIMEOUT}s)" >&2
STDERR_PATH="$OUT_DIR/${LABEL}.stderr"
# Not "if ! timeout ...; then status=$?": under set -e, $? right after entering
# a negated `if !` condition reflects the negation's own result, not the
# original command's exit code (it reads as 0 even when the command failed).
set +e
timeout "${TIMEOUT}s" "$BINARY" "${BINARY_ARGS[@]}" > "$LOG_PATH" 2> "$STDERR_PATH"
status=$?
set -e
if [[ "$status" -ne 0 ]]; then
  if [[ "$status" -eq 124 ]]; then
    echo "HANG DETECTED: '$BINARY' exceeded ${TIMEOUT}s timeout — see $LOG_PATH (partial)" >&2
  else
    echo "'$BINARY' exited with status $status — see $STDERR_PATH" >&2
  fi
  exit "$status"
fi
cat "$STDERR_PATH" >&2
echo "wrote $LOG_PATH" >&2

if [[ "$NO_ANALYZE" -eq 1 ]]; then
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/analyze_propagation.py" \
  --log "$LOG_PATH" --label "$LABEL" \
  --total-pieces "$TOTAL_PIECES" --piece-bytes "$PIECE_BYTES" --receiver "$RECEIVER" \
  --out-dir "$OUT_DIR" "${SOCKET_ARGS[@]}" "$@"
