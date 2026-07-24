// SPDX-License-Identifier: BSD-2-Clause
/**
 * Unit tests for piece_selector, driven directly (no coordinator),
 * mirroring test_peer_wire.cpp/test_choking_policy.cpp's direct-call
 * style. Covers the bead's acceptance bar: rule precedence
 * (random-first vs rarest-first, tie-break lowest index) and endgame
 * entry/cancel logic.
 */
#include <cadmium/modeling/message_box.hpp>

#include "../models/client/piece_selector.hpp"
#include "../msg/peer_id.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

    using bt_utp::obs_availability;
    using bt_utp::obs_completion;
    using bt_utp::peer_id;
    using bt_utp::piece_selector;
    using bt_utp::request_plan;
    using bt_utp::sub_piece_id;

    using ps_t   = piece_selector<double>;
    using defs   = ps_t::defs;
    using box_in = cadmium::make_message_box<ps_t::input_ports>::type;

    box_in avail_box(peer_id peer, std::vector<bool> bitfield) {
        box_in box;
        cadmium::get_message<defs::obs_in>(box).emplace(
            obs_availability{peer, std::move(bitfield)});
        return box;
    }
    box_in completion_box(peer_id peer, std::uint32_t piece_index, std::uint32_t sub_index,
                          bool full_piece) {
        box_in box;
        cadmium::get_message<defs::obs_in>(box).emplace(
            obs_completion{peer, piece_index, full_piece, sub_index});
        return box;
    }

    /// Drains every pending request_plan by alternating output()/
    /// internal_transition() until time_advance() stops being 0.
    std::vector<request_plan> drain(ps_t &ps) {
        std::vector<request_plan> out;
        while (ps.time_advance() == 0.0) {
            auto box = ps.output();
            if (const auto &m = cadmium::get_message<defs::plan_out>(box); m.has_value()) {
                out.push_back(*m);
            }
            ps.internal_transition();
        }
        return out;
    }

} // namespace

TEST_CASE("piece_selector: constructs and validates its geometry parameters") {
    ps_t ps(1, 1);
    CHECK(ps.time_advance() == std::numeric_limits<double>::infinity());
    CHECK_THROWS_AS(ps_t(0, 1), std::invalid_argument);
    CHECK_THROWS_AS(ps_t(1, 0), std::invalid_argument);
}

TEST_CASE("piece_selector: random-first picks the lowest-index piece while we have none complete") {
    ps_t ps(3, 1); // 3 pieces, 1 sub-piece each -- simplifies assertions
    ps.external_transition(0.0, avail_box(1, {true, true, true}));
    auto plans = drain(ps);
    REQUIRE(plans.size() == 1);
    CHECK(plans[0].peer == 1);
    REQUIRE(plans[0].items.size() == 1);
    CHECK(plans[0].items[0] == sub_piece_id{0, 0}); // lowest index, not rarest
}

TEST_CASE("piece_selector: rarest-first picks the lowest-availability piece once we have one "
          "complete, tie-break lowest index") {
    ps_t ps(3, 1);
    // Mark piece 0 complete directly (bypasses needing a full selection
    // round-trip just to set up completed_count > 0).
    ps.external_transition(0.0, completion_box(1, 0, 0, /*full_piece=*/true));
    drain(ps); // nothing pending yet (peer 1's bitfield was never given)

    // Piece 1 available from only one peer; piece 2 available from two --
    // piece 1 is rarer.
    ps.external_transition(0.0, avail_box(10, {true, true, false}));
    drain(ps); // peer 10 has nothing to offer yet relative to itself below
    ps.external_transition(0.0, avail_box(11, {true, false, true}));
    drain(ps);
    ps.external_transition(0.0, avail_box(12, {true, false, true}));
    drain(ps);

    // A fresh peer with both piece 1 and piece 2 available: must pick the
    // rarer piece 1 (availability 1), not piece 2 (availability 2).
    ps.external_transition(0.0, avail_box(20, {true, true, true}));
    auto plans = drain(ps);
    REQUIRE(plans.size() == 1);
    CHECK(plans[0].peer == 20);
    REQUIRE(plans[0].items.size() == 1);
    CHECK(plans[0].items[0] == sub_piece_id{1, 0});
}

TEST_CASE("piece_selector: strict priority + endgame lets a new peer join an already-started "
          "piece ahead of starting a fresh one, and delivery cancels the redundant copy") {
    ps_t ps(2, 1); // 2 pieces, 1 sub-piece each
    // Peer A gets piece 0 (random-first, lowest index).
    ps.external_transition(0.0, avail_box(1, {true, true}));
    auto plan_a = drain(ps);
    REQUIRE(plan_a.size() == 1);
    CHECK(plan_a[0].peer == 1);
    CHECK(plan_a[0].items == std::vector<sub_piece_id>{{0, 0}});

    // Peer B only has piece 1: tier 2 assigns it piece 1 (the only
    // candidate) -- now both pieces are started and fully assigned, so
    // in_endgame() becomes true.
    ps.external_transition(0.0, avail_box(2, {false, true}));
    auto plan_b = drain(ps);
    REQUIRE(plan_b.size() == 1);
    CHECK(plan_b[0].peer == 2);
    CHECK(plan_b[0].items == std::vector<sub_piece_id>{{1, 0}});

    // Peer C has both pieces. Strict priority (tier 1) must claim piece 0
    // (already started, ascending-order first) for the redundant
    // endgame assignment -- not piece 1, and not "nothing" just because
    // both pieces are technically already fully assigned.
    ps.external_transition(0.0, avail_box(3, {true, true}));
    auto plan_c = drain(ps);
    REQUIRE(plan_c.size() == 1);
    CHECK(plan_c[0].peer == 3);
    CHECK(plan_c[0].items == std::vector<sub_piece_id>{{0, 0}});

    // Peer A (the original) delivers piece 0's only sub-piece first: the
    // redundant copy requested from peer C must now be cancelled.
    ps.external_transition(0.0, completion_box(1, 0, 0, /*full_piece=*/true));
    auto plans_after = drain(ps);
    // Two things happen off this one completion: the redundant copy
    // requested from peer C gets cancelled, *and* peer A (now free, and
    // we're still in endgame since piece 1 isn't delivered yet) picks up
    // piece 1's sub-piece redundantly too -- both are correct, not a bug.
    REQUIRE(plans_after.size() == 2);
    const request_plan *cancellation = nullptr;
    const request_plan *reassignment = nullptr;
    for (const auto &p : plans_after) {
        if (!p.cancellations.empty()) {
            cancellation = &p;
        } else if (!p.items.empty()) {
            reassignment = &p;
        }
    }
    REQUIRE(cancellation != nullptr);
    CHECK(cancellation->peer == 3);
    CHECK(cancellation->items.empty());
    CHECK(cancellation->cancellations == std::vector<sub_piece_id>{{0, 0}});

    REQUIRE(reassignment != nullptr);
    CHECK(reassignment->peer == 1);
    CHECK(reassignment->items == std::vector<sub_piece_id>{{1, 0}});
}

TEST_CASE("piece_selector: a completion from a peer we've never heard availability from doesn't "
          "crash or emit anything") {
    ps_t ps(1, 1);
    ps.external_transition(0.0, completion_box(99, 0, 0, /*full_piece=*/false));
    CHECK(drain(ps).empty());
}
