// SPDX-License-Identifier: BSD-2-Clause
/**
 * Unit tests for choking_policy, driven directly (no coordinator),
 * mirroring test_peer_wire.cpp's direct-call style: build a message box,
 * call external_transition/output/internal_transition, inspect state.
 * Covers the bead's acceptance bar: rate ranking (top-4), snub trigger at
 * exactly 60s, metric switch on completion, round-robin rotation.
 */
#include <cadmium/modeling/message_box.hpp>

#include "../models/client/choking_policy.hpp"
#include "../msg/peer_id.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

    using bt_utp::choke_cmd;
    using bt_utp::choking_policy;
    using bt_utp::obs_completion;
    using bt_utp::obs_peer_interest;
    using bt_utp::obs_upload;
    using bt_utp::peer_id;

    using cp_t   = choking_policy<double>;
    using defs   = cp_t::defs;
    using box_in = cadmium::make_message_box<cp_t::input_ports>::type;

    box_in interest_box(peer_id peer, bool interested) {
        box_in box;
        cadmium::get_message<defs::obs_in>(box).emplace(obs_peer_interest{peer, interested});
        return box;
    }
    box_in completion_box(peer_id peer, std::uint32_t piece_index, bool full_piece) {
        box_in box;
        cadmium::get_message<defs::obs_in>(box).emplace(
            obs_completion{peer, piece_index, full_piece});
        return box;
    }
    box_in upload_box(peer_id peer, std::uint32_t piece_index, std::uint32_t bytes) {
        box_in box;
        cadmium::get_message<defs::obs_in>(box).emplace(obs_upload{peer, piece_index, bytes});
        return box;
    }

    /// Drains every pending choke_cmd by alternating output()/
    /// internal_transition() until time_advance() stops being 0 —
    /// matching the "a due timer can legitimately output nothing" pattern
    /// (see choking_policy.hpp): this collects whatever *did* get queued
    /// without assuming exactly one command per step.
    std::vector<choke_cmd> drain(cp_t &cp) {
        std::vector<choke_cmd> out;
        while (cp.time_advance() == 0.0) {
            auto box = cp.output();
            if (const auto &m = cadmium::get_message<defs::choke_cmd_out>(box); m.has_value()) {
                out.push_back(*m);
            }
            cp.internal_transition();
        }
        return out;
    }

    /// Advances state.now to the next due rechoke/optimistic timer,
    /// exercising the exact calling contract a coordinator would use: the
    /// step where the timer itself fires must see output() legitimately
    /// return an empty box (nothing was queued *yet* -- see
    /// choking_policy.hpp's internal_transition() comment), and only the
    /// commands that timer produces (queued for the *next* step) get
    /// collected by the drain() that follows.
    std::vector<choke_cmd> advance_to_next_timer(cp_t &cp) {
        REQUIRE(cp.time_advance() > 0.0); // not already imminent from a leftover command
        auto box = cp.output();
        CHECK_FALSE(cadmium::get_message<defs::choke_cmd_out>(box).has_value());
        cp.internal_transition(); // consumes the timer's sigma; may populate pending
        return drain(cp);
    }

} // namespace

TEST_CASE("choking_policy: constructs and validates its geometry parameters") {
    cp_t cp(1, 100.0);
    // Always has a next rechoke/optimistic deadline scheduled from
    // construction on, so unlike peer_wire/utp_socket this atomic never
    // reports time_advance() == infinity -- there's no genuinely passive
    // state for it to be called on out of turn.
    CHECK(cp.time_advance() == 10.0); // the 10s rechoke period, the sooner of the two timers
    CHECK_THROWS_AS(cp_t(0, 100.0), std::invalid_argument);
    CHECK_THROWS_AS(cp_t(1, 0.0), std::invalid_argument);
}

TEST_CASE("choking_policy: rate ranking unchokes only the top 4 by rolling rx-rate") {
    cp_t cp(1, 100.0);
    // Five interested peers, fed distinct one-shot download rates: peer 5
    // gets the least (lowest, excluded), 1..4 get decreasing amounts.
    for (peer_id peer = 1; peer <= 5; ++peer) {
        cp.external_transition(0.0, interest_box(peer, true));
    }
    // Peer N receives (6-N) sub-pieces immediately (peer 1: 5, peer 5: 1),
    // ordering rate strictly 1 > 2 > 3 > 4 > 5.
    for (peer_id peer = 1; peer <= 5; ++peer) {
        for (std::uint32_t i = 0; i < (6 - peer); ++i) {
            cp.external_transition(0.0, completion_box(peer, i, false));
        }
    }

    auto cmds = advance_to_next_timer(cp); // first rechoke @10s
    std::vector<peer_id> unchoked;
    for (const auto &c : cmds) {
        if (c.unchoke) {
            unchoked.push_back(c.peer);
        }
    }
    REQUIRE(unchoked.size() == 4);
    CHECK(std::find(unchoked.begin(), unchoked.end(), 5) == unchoked.end()); // lowest-rate excluded
    for (peer_id peer = 1; peer <= 4; ++peer) {
        CHECK(std::find(unchoked.begin(), unchoked.end(), peer) != unchoked.end());
    }
}

TEST_CASE("choking_policy: a peer is snubbed at exactly 60s without data, not before") {
    cp_t cp(1, 100.0);
    cp.external_transition(0.0, interest_box(7, true));
    // One sub-piece so this peer is initially rate-unchoked.
    cp.external_transition(0.0, completion_box(7, 0, false));
    advance_to_next_timer(cp); // rechoke @10s: peer 7 unchoked
    REQUIRE(cp.state.peers.at(7).unchoked);

    // 4 more rechokes with no further data: 20s, 30s, 40s, 50s.
    for (int i = 0; i < 4; ++i) {
        advance_to_next_timer(cp);
    }
    CHECK_FALSE(cp.state.peers.at(7).snubbed); // 50s: not yet

    advance_to_next_timer(cp); // 60s: exactly at the snub threshold
    CHECK(cp.state.peers.at(7).snubbed);
}

TEST_CASE("choking_policy: ranking metric switches to upload-rate once our download completes") {
    cp_t cp(1, 100.0); // total_pieces = 1: one full-piece completion flips have_complete_file
    cp.external_transition(0.0, interest_box(7, true));

    // Complete our only piece -- have_complete_file flips true. The
    // completion that *causes* the flip still counts as a download
    // sample itself (the switch only affects events after it).
    cp.external_transition(0.0, completion_box(7, 0, /*full_piece=*/true));
    CHECK(cp.state.have_complete_file);
    REQUIRE(cp.state.peers.at(7).rate_samples.size() == 1);

    // A further obs_completion (download direction) must NOT feed the
    // rate window anymore; an obs_upload must.
    cp.external_transition(0.0, completion_box(7, 0, false));
    CHECK(cp.state.peers.at(7).rate_samples.size() == 1); // unchanged: post-flip download ignored
    cp.external_transition(0.0, upload_box(7, 0, 500));
    REQUIRE(cp.state.peers.at(7).rate_samples.size() == 2);
    CHECK(cp.state.peers.at(7).rate_samples.back().second == 500.0);
}

TEST_CASE("choking_policy: the optimistic slot rotates round-robin, newcomers first") {
    cp_t cp(1, 100.0);
    // Peers 1-4 get a fresh sub-piece before every timer check, keeping
    // them both rate-unchoked (real BEP 3 unchokes the top-N once there's
    // no better candidate, even at a modest rate) *and* un-snubbed (this
    // test is about the optimistic rotation specifically, not the 60s
    // snub timeout the other test above already covers). Peers 5-7 are
    // interested but never get any data at all, so they're excess beyond
    // the rate-ranked top-4 -- eligible only via the optimistic slot,
    // which is what this test actually exercises.
    auto feed_top4 = [&]() {
        for (peer_id peer = 1; peer <= 4; ++peer) {
            cp.external_transition(0.0, completion_box(peer, 0, false));
        }
    };

    for (peer_id peer = 1; peer <= 7; ++peer) {
        cp.external_transition(0.0, interest_box(peer, true));
    }
    feed_top4();
    advance_to_next_timer(cp); // rechoke @10s: peers 1-4 rate-unchoked
    for (peer_id peer = 1; peer <= 4; ++peer) {
        CHECK(cp.state.rate_unchoked.contains(peer));
    }

    // First optimistic rotation @30s: registration order was 1..7, so
    // newcomer-first rotation is [7,6,5,4,3,2,1]; filtering out the
    // rate-unchoked 1-4 leaves eligible = [7,6,5] -- rotation starts at
    // index 0 -> peer 7.
    feed_top4();
    advance_to_next_timer(cp); // rechoke @20s: no change
    feed_top4();
    advance_to_next_timer(cp); // rechoke @30s *and* optimistic @30s
    REQUIRE(cp.state.optimistic.has_value());
    CHECK(*cp.state.optimistic == 7);

    feed_top4();
    advance_to_next_timer(cp); // rechoke @40s
    feed_top4();
    advance_to_next_timer(cp); // rechoke @50s
    feed_top4();
    auto rotation_2 = advance_to_next_timer(cp); // rechoke+optimistic @60s: rotates to peer 6
    REQUIRE(cp.state.optimistic.has_value());
    CHECK(*cp.state.optimistic == 6);
    // The rotation must have choked 7 and unchoked 6; peers 1-4 (still
    // rate-unchoked throughout) must not appear in this diff at all.
    bool choked_7 = false, unchoked_6 = false;
    for (const auto &c : rotation_2) {
        CHECK(c.peer != 1);
        CHECK(c.peer != 2);
        CHECK(c.peer != 3);
        CHECK(c.peer != 4);
        if (c.peer == 7 && !c.unchoke) {
            choked_7 = true;
        }
        if (c.peer == 6 && c.unchoke) {
            unchoked_6 = true;
        }
    }
    CHECK(choked_7);
    CHECK(unchoked_6);
}
