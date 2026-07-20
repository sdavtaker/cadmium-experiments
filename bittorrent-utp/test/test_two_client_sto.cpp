// SPDX-License-Identifier: BSD-2-Clause
/**
 * Stochastic-pass ctests for the two-client uTP scenario (lossy_channel in
 * place of bottleneck_channel): seed reproducibility and the reliability
 * invariant (every byte still arrives despite loss/jitter) across the p
 * sweep {0.001, 0.01, 0.05}. Throughput-vs-p monotonicity across many seeds
 * is a statistical trend, not a single-run assertion — that is checked by
 * scripts/throughput_vs_loss.py against the bt-utp-s2-sto executable
 * (same run_two_client_sto() this file drives directly, no process spawn
 * needed here).
 */
#include "../models/utp/two_client_sto_scenario.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <random>

using bt_utp::run_two_client_sto;
using bt_utp::sto_scenario_result;

TEST_CASE("sto two-client: same seed reproduces an identical result") {
    const sto_scenario_result r1 = run_two_client_sto(1234, 0.02);
    const sto_scenario_result r2 = run_two_client_sto(1234, 0.02);

    CHECK(r1.finish == r2.finish);
    CHECK(r1.all_delivered == r2.all_delivered);
    CHECK(r1.dropped_ab == r2.dropped_ab);
    CHECK(r1.retransmits_a == r2.retransmits_a);
    CHECK(r1.cwnd_final_a == r2.cwnd_final_a);
}

TEST_CASE("sto two-client: every byte is delivered despite loss, for every p in the sweep") {
    // {0.001, 0.01, 0.05} per fable_plan.md's stage-2 sto-pass reliability
    // invariant. A handful of seeds per p, not an exhaustive search: the
    // point is that uTP's retransmission logic repairs loss regardless of
    // which packets happen to drop, not that any specific seed is special.
    for (double p : {0.001, 0.01, 0.05}) {
        for (std::uint32_t seed = 1; seed <= 5; ++seed) {
            const sto_scenario_result r = run_two_client_sto(seed, p);
            INFO("p=" << p << " seed=" << seed << " finish=" << r.finish
                      << " dropped_ab=" << r.dropped_ab << " retransmits=" << r.retransmits_a);
            CHECK(r.all_delivered);
        }
    }
}

TEST_CASE("sto two-client: higher loss probability visibly drives more retransmits") {
    // Not a strict per-seed monotonicity claim (that's the scripted,
    // many-seed statistical check) — just confirms loss actually engages
    // the repair path at all, at a fixed seed.
    const sto_scenario_result low  = run_two_client_sto(99, 0.001);
    const sto_scenario_result high = run_two_client_sto(99, 0.05);
    CHECK(low.all_delivered);
    CHECK(high.all_delivered);
    CHECK(high.retransmits_a >= low.retransmits_a);
}
