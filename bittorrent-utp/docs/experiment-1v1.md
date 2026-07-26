# Experiment 1v1: stage-3 fixed-pattern vs stage-4 real-policy (deterministic)

Bead: `zqns`. Two-client seed/leech BitTorrent/uTP transfer, run once with
stage-3's stub policies (`stub_always_unchoke` + `stub_sequential_selector`)
and once with stage-4's real atomics (`choking_policy` + `piece_selector`),
on identically-parameterized channels — same `total_pieces` (40),
`sub_pieces_per_piece` (16), `sub_piece_bytes` (16 KiB), `rate_ab` (1 MB/s),
`rate_ba` (100 MB/s), and `prop_delay` (0.05s) in both builds (see
`models/client/s3_two_client_det.hpp` / `s4_two_client_det.hpp`).

Runs produced with `scripts/run_experiment.sh`, analyzed with
`scripts/analyze_propagation.py` (bead `zgmx`):

```
scripts/run_experiment.sh --binary ../build/bittorrent-utp/bt-utp-s3-det \
    --label s3-fixed-pattern --t-max 60 --total-pieces 40 --piece-bytes 262144 \
    --receiver client_b.wire --out-dir docs --timeout 300

scripts/run_experiment.sh --binary ../build/bittorrent-utp/bt-utp-s4-det \
    --label s4-policy --t-max 60 --total-pieces 40 --piece-bytes 262144 \
    --receiver client_b.wire --out-dir docs --timeout 300
```

## Before / after

| | stage-3 (fixed-pattern) | stage-4 (real policy) | Δ |
|---|---:|---:|---:|
| Propagation time (sim-s) | 47.4424 | 50.9773 | +3.5349 |
| Pieces completed | 40/40 | 40/40 | — |
| REQUEST messages | 1280 | 1280 | 0 |
| PIECE messages | 1280 | 1280 | 0 |
| HAVE messages | 80 | 80 | 0 |
| UNCHOKE messages | 3 | 3 | 0 |
| Total messages | 2655 | 2655 | 0 |

Full per-run detail: [`s3-fixed-pattern_results.json`](s3-fixed-pattern_results.json),
[`s4-policy_results.json`](s4-policy_results.json). Delivery-rate time series:
[`s3-fixed-pattern_rate.svg`](s3-fixed-pattern_rate.svg),
[`s4-policy_rate.svg`](s4-policy_rate.svg) (CSV alongside each).

## Reading the numbers

**Propagation time** (sim-time from t=0 content availability at the seed to
the receiver's last piece-completion event) is **3.5349s higher** under the
real policy. This is a decision-timer effect, not a protocol-efficiency
regression: `choking_policy`'s first rechoke tick fires 10s after
construction, briefly delaying clientA's first unchoke of clientB relative
to stage-3's stub (which unchokes unconditionally from t=0). Once unchoked,
the transfer proceeds identically — every other metric in the table matches
exactly.

**Message counts are byte-for-byte identical** between the two runs. Neither
client is ever actually choked in this simple seed/leech topology (both
sides stay interested throughout the transfer), so swapping in the real
`choking_policy`/`piece_selector` atomics changes *when* decisions happen but
not *how many* protocol messages the transfer takes. This is the expected
result for bead `693b`'s "swap stubs for real atomics, type change only"
premise holding at the protocol level — real choking/selection logic doesn't
change the wire-level exchange in a topology where nothing ever gets choked.

## Regression guard

`test/test_s5_det_comparison.cpp` pins this comparison as a ctest: asserts
the stage-4/stage-3 overhead is bounded (`0 <= overhead <= 20s`, a generous
ceiling above the measured ~3.5s that still catches a genuine slowdown) and
that REQUEST/PIECE/HAVE counts match exactly between the two builds. Neither
`test_s3_two_client_det.cpp` nor `test_s4_two_client_det.cpp` checks the
*relationship* between the two stages on its own — each only pins its own
stage's completion time individually.

## Not covered here

- **Stochastic pass** (lossy channels, seeded replications, confidence
  intervals): bead `ki1r`, blocked on `8jfl` (stochastic policy pass).
- **cwnd/rate-over-time detail beyond delivery rate**: available via
  `run_experiment.sh --socket client_a.socket --socket client_b.socket`
  (implies `--full-trace`), not generated here to keep this experiment's
  cost down — see `scripts/run_experiment.sh`'s own comment on why
  `--full-trace` multiplies trace volume/runtime substantially.
