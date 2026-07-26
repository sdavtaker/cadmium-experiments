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

**Propagation time** is **3.5349s higher** under the real policy overall,
but that net number hides two effects pulling in opposite directions. The
delivery-rate series ([`s3-fixed-pattern_rate.svg`](s3-fixed-pattern_rate.svg)
vs [`s4-policy_rate.svg`](s4-policy_rate.svg)) shows stage-3 starts
delivering at ~t=1s (`stub_always_unchoke` unchokes unconditionally from
t=0) while stage-4 doesn't start until ~t=11s — `choking_policy`'s first
rechoke tick fires 10s after construction, gating clientA's first unchoke
of clientB. That's a ~10s later start, yet stage-4 only ends up ~3.5s
behind overall: its active transfer phase (once unchoked) is itself
*faster* than stage-3's stub-driven one, clawing back roughly 6.5s of that
later start. Real `piece_selector` evidently pipelines requests more
effectively than `stub_sequential_selector` once both sides are unchoked —
this experiment doesn't isolate exactly how much of the 6.5s comes from
selection order versus other request/response timing, only that the net
effect is real and reproducible.

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
that REQUEST/PIECE/HAVE counts match exactly between the two builds. Both
sides use the same completion metric (sim_time of the receiver's Nth
distinct HAVE) so the delta is apples-to-apples — deliberately not
`test_s3_two_client_det.cpp`'s own "largest sim_time anywhere in the trace"
measure, which includes a real ~0.1s post-completion settling tail that
would otherwise bias a cross-stage comparison. Neither
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
