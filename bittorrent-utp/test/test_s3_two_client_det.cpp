// SPDX-License-Identifier: BSD-2-Clause
/**
 * Deterministic two-client end-to-end exchange: clientA (seed, full
 * bitfield) transfers all 40 pieces to clientB (empty) over two real
 * bottleneck_channel instances, driving two real bittorrent_client
 * couplings through cadmium's own runner (no hand-rolled scheduler).
 */
#include "../models/client/s3_two_client_det.hpp"
#include <catch2/catch_test_macros.hpp>
#include <limits>

TEST_CASE("s3 det: smoke test — runs to completion without crashing") {
    const auto result = bt_utp::run_s3_det();
    // runner.run_until() returns the *next scheduled* time, not the last
    // processed one — once the transfer completes and both clients settle
    // (A never requests anything as a full seed; B stops once it has
    // everything, per peer_wire's own-completion interest recompute),
    // nothing remains scheduled and this correctly becomes infinity. That
    // is the actual "done, no hang" signal here, not a failure to finish
    // within the time budget.
    CHECK(result.finish == std::numeric_limits<double>::infinity());
    CHECK(!result.ndjson_log.empty());
}
