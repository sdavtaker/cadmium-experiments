// SPDX-License-Identifier: BSD-2-Clause
/**
 * Unit tests for peer_wire (BEP 3 per-connection FSM), driven directly
 * (no coordinator/socket), mirroring test_utp_socket.cpp's direct-call
 * style: build a message box, call external_transition/output/
 * internal_transition, inspect state. Covers the deterministic-pass FSM
 * acceptance bar: illegal transitions rejected, interest flags always
 * consistent with bitfield/have deltas.
 */
#include <cadmium/modeling/message_box.hpp>

#include "../models/client/peer_wire.hpp"
#include "../msg/bep3.hpp"
#include "../msg/peer_id.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace {

    using bt_utp::bep3_msg;
    using bt_utp::bitfield;
    using bt_utp::cancel;
    using bt_utp::choke;
    using bt_utp::choke_cmd;
    using bt_utp::handshake;
    using bt_utp::have;
    using bt_utp::interested;
    using bt_utp::not_interested;
    using bt_utp::peer_wire;
    using bt_utp::piece;
    using bt_utp::request;
    using bt_utp::request_plan;
    using bt_utp::sub_piece_id;
    using bt_utp::unchoke;
    using bt_utp::wire_msg;

    using pw_t   = peer_wire<double>;
    using defs   = pw_t::defs;
    using box_in = cadmium::make_message_box<pw_t::input_ports>::type;

    box_in wire_in_box(bt_utp::peer_id src, wire_msg m) {
        box_in box;
        cadmium::get_message<defs::wire_in>(box).emplace(
            typename defs::wire_deliver_ind{src, std::move(m)});
        return box;
    }
    box_in choke_cmd_box(bt_utp::peer_id peer, bool unchoke_) {
        box_in box;
        cadmium::get_message<defs::choke_cmd_in>(box).emplace(choke_cmd{peer, unchoke_});
        return box;
    }
    box_in plan_box(bt_utp::peer_id peer, std::vector<sub_piece_id> items) {
        box_in box;
        cadmium::get_message<defs::plan_in>(box).emplace(request_plan{peer, std::move(items)});
        return box;
    }

    /// Drains every pending (wire_out, obs_out) message by alternating
    /// output()/internal_transition() until both queues are empty,
    /// collecting each into a flat vector for easy assertions.
    std::vector<std::pair<bt_utp::peer_id, wire_msg>> drain_wire(pw_t &pw) {
        std::vector<std::pair<bt_utp::peer_id, wire_msg>> out;
        while (pw.time_advance() == 0.0) {
            auto box = pw.output();
            if (const auto &m = cadmium::get_message<defs::wire_out>(box); m.has_value()) {
                out.emplace_back(m->dst, m->payload);
            }
            pw.internal_transition();
        }
        return out;
    }

    /// Same draining pattern as drain_wire, collecting obs_out instead —
    /// both queues advance in lockstep per output()/internal_transition()
    /// call regardless of which one this collects from.
    std::vector<bt_utp::peer_wire_obs> drain_obs(pw_t &pw) {
        std::vector<bt_utp::peer_wire_obs> out;
        while (pw.time_advance() == 0.0) {
            auto box = pw.output();
            if (const auto &m = cadmium::get_message<defs::obs_out>(box); m.has_value()) {
                out.push_back(*m);
            }
            pw.internal_transition();
        }
        return out;
    }

} // namespace

TEST_CASE("peer_wire: output()/internal_transition() reject being called while passive") {
    // A correctly-functioning coordinator only calls these when
    // time_advance() == 0; calling them on a passive model is a DEVS
    // calling-contract violation this atomic must surface loudly rather
    // than silently no-op.
    pw_t pw(2, 2, 100, {false, false}); // no connect_to: passive from construction
    REQUIRE(pw.time_advance() == std::numeric_limits<double>::infinity());
    CHECK_THROWS_AS(pw.output(), std::logic_error);
    CHECK_THROWS_AS(pw.internal_transition(), std::logic_error);
}

TEST_CASE("peer_wire: constructs and validates its geometry parameters") {
    peer_wire<double> pw(4, 2, 100, {true, true, true, true});
    CHECK(pw.time_advance() == std::numeric_limits<double>::infinity());
    CHECK_THROWS_AS(pw_t(4, 0, 100, {false, false, false, false}), std::invalid_argument);
    CHECK_THROWS_AS(pw_t(4, 2, 0, {false, false, false, false}), std::invalid_argument);
    CHECK_THROWS_AS(pw_t(4, 2, 100, {false, false, false}), std::invalid_argument); // size mismatch
}

TEST_CASE("peer_wire: active-open emits handshake then bitfield at construction") {
    pw_t pw(2, 2, 100, {true, true}, /*connect_to=*/7);
    auto sent = drain_wire(pw);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].first == 7);
    CHECK(std::holds_alternative<handshake>(sent[0].second));
    CHECK(sent[1].first == 7);
    REQUIRE(std::holds_alternative<bep3_msg>(sent[1].second));
    const auto &bf = std::get<bitfield>(std::get<bep3_msg>(sent[1].second));
    CHECK(bf.pieces == std::vector<bool>{true, true});
}

TEST_CASE("peer_wire: acceptor role replies in kind to an unsolicited handshake") {
    pw_t pw(2, 2, 100, {false, false}); // no connect_to: passive
    CHECK(pw.time_advance() == std::numeric_limits<double>::infinity());

    pw.external_transition(0.0, wire_in_box(9, wire_msg{handshake{}}));
    auto sent = drain_wire(pw);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].first == 9);
    CHECK(std::holds_alternative<handshake>(sent[0].second));
    CHECK(std::holds_alternative<bep3_msg>(sent[1].second));
}

TEST_CASE("peer_wire: duplicate handshake on an established connection is rejected") {
    pw_t pw(2, 2, 100, {false, false}, /*connect_to=*/7);
    drain_wire(pw); // our own handshake+bitfield

    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}})); // their (first) handshake
    auto first_reply = drain_wire(pw);
    CHECK(first_reply.empty()); // we already sent ours at construction; no re-send

    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}})); // illegal: duplicate
    auto second_reply = drain_wire(pw);
    CHECK(second_reply.empty());
}

TEST_CASE("peer_wire: any message before a handshake is rejected") {
    pw_t pw(2, 2, 100, {false, false}); // passive, no conn yet
    pw.external_transition(0.0, wire_in_box(9, wire_msg{bep3_msg{choke{}}}));
    CHECK(drain_wire(pw).empty());
    CHECK(pw.time_advance() == std::numeric_limits<double>::infinity());
}

TEST_CASE("peer_wire: interest flag is recomputed from a bitfield delta") {
    pw_t pw(2, 2, 100, {true, false}, /*connect_to=*/7); // we hold piece 0, miss piece 1
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);

    // Peer has piece 1 (which we miss) -> we must become interested.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{bitfield{{false, true}}}}));
    auto sent = drain_wire(pw);
    REQUIRE(sent.size() == 1);
    REQUIRE(std::holds_alternative<bep3_msg>(sent[0].second));
    CHECK(std::holds_alternative<interested>(std::get<bep3_msg>(sent[0].second)));

    // Peer's bitfield now only covers what we already have -> not interested.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{bitfield{{true, false}}}}));
    auto sent2 = drain_wire(pw);
    REQUIRE(sent2.size() == 1);
    CHECK(std::holds_alternative<not_interested>(std::get<bep3_msg>(sent2[0].second)));
}

TEST_CASE("peer_wire: interest flag is recomputed from a have delta") {
    pw_t pw(2, 2, 100, {true, true}, /*connect_to=*/7); // we hold everything already
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{bitfield{{true, true}}}}));
    CHECK(drain_wire(pw).empty()); // nothing they have is something we miss

    // A have for a piece we don't have would matter; here both already
    // have everything, so a have message must not spuriously flip interest.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{have{0}}}));
    CHECK(drain_wire(pw).empty());
}

TEST_CASE("peer_wire: have/bitfield indices beyond total_pieces are rejected") {
    pw_t pw(2, 2, 100, {false, false}, /*connect_to=*/7); // total_pieces == 2
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);

    // have{2} is out of range for a 2-piece swarm (valid indices: 0, 1).
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{have{2}}}));
    CHECK(drain_wire(pw).empty()); // rejected: no interest flip, nothing queued

    // A 3-entry bitfield for a 2-piece swarm is likewise out of range.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{bitfield{{true, true, true}}}}));
    CHECK(drain_wire(pw).empty());
}

TEST_CASE("peer_wire: interest flag is recomputed after our own piece completion") {
    // Regression test: our own have[] changing (via a completed download)
    // is a have delta exactly like the peer's, and must re-trigger
    // recompute_interest — otherwise am_interested stays stuck true even
    // after we've acquired everything that made us interested.
    pw_t pw(1, 1, 100, {false}, /*connect_to=*/7); // we have nothing yet
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);

    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{bitfield{{true}}}}));
    auto interested_sent = drain_wire(pw);
    REQUIRE(interested_sent.size() == 1);
    CHECK(std::holds_alternative<interested>(std::get<bep3_msg>(interested_sent[0].second)));

    pw.external_transition(0.0, plan_box(7, {{0, 0}}));
    CHECK(drain_wire(pw).empty()); // still choked by them: not requested yet
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{unchoke{}}}));
    drain_wire(pw); // the request, now that they've unchoked us

    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{piece{0, 0, 100}}}));
    auto sent = drain_wire(pw);
    // `have` (to the peer) and `not_interested` (nothing left to want).
    bool saw_not_interested = false;
    for (const auto &[dst, msg] : sent) {
        if (std::holds_alternative<bep3_msg>(msg) &&
            std::holds_alternative<not_interested>(std::get<bep3_msg>(msg))) {
            saw_not_interested = true;
        }
    }
    CHECK(saw_not_interested);
}

TEST_CASE("peer_wire: unchoke fills the request pipeline up to pipeline_depth") {
    pw_t pw(3, 4, 100, {false, false, false}, /*connect_to=*/7);
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);

    const std::vector<sub_piece_id> items = {{0, 0}, {0, 1}, {0, 2}, {0, 3},
                                             {1, 0}, {1, 1}, {1, 2}, {1, 3}};
    pw.external_transition(0.0, plan_box(7, items));
    CHECK(drain_wire(pw).empty()); // still choked by them: nothing requested yet

    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{unchoke{}}}));
    auto sent = drain_wire(pw);
    REQUIRE(sent.size() == pw_t::pipeline_depth); // capped at 5, 3 remain planned
    for (const auto &[dst, msg] : sent) {
        CHECK(dst == 7);
        REQUIRE(std::holds_alternative<bep3_msg>(msg));
        CHECK(std::holds_alternative<request>(std::get<bep3_msg>(msg)));
    }
}

TEST_CASE("peer_wire: choke requeues outstanding requests instead of dropping them") {
    pw_t pw(1, 2, 100, {false}, /*connect_to=*/7);
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);
    pw.external_transition(0.0, plan_box(7, {{0, 0}, {0, 1}}));
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{unchoke{}}}));
    REQUIRE(drain_wire(pw).size() == 2); // both requested

    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{choke{}}}));
    CHECK(drain_wire(pw).empty()); // choke itself sends nothing

    // Unchoke again: the same two requests must be reissued from planned.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{unchoke{}}}));
    auto resent = drain_wire(pw);
    CHECK(resent.size() == 2);
}

TEST_CASE("peer_wire: a request is ignored while we are choking the peer") {
    pw_t pw(1, 1, 100, {true}); // seed of 1 piece, passive/acceptor
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw); // our handshake+bitfield reply

    // Default am_choking == true: the request must be ignored.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{request{0, 0, 100}}}));
    CHECK(drain_wire(pw).empty());
    CHECK(drain_obs(pw).empty()); // no obs_upload for a request we didn't serve
}

TEST_CASE("peer_wire: a request for a piece we don't have is ignored, even unchoked") {
    pw_t pw(1, 1, 100, {false}); // we have nothing
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);
    pw.external_transition(0.0, choke_cmd_box(7, /*unchoke=*/true));
    CHECK(drain_wire(pw).size() == 1); // the outgoing `unchoke` itself

    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{request{0, 0, 100}}}));
    CHECK(drain_wire(pw).empty()); // illegal: we don't have piece 0
    CHECK(drain_obs(pw).empty());  // no obs_upload for a request we didn't serve
}

TEST_CASE("peer_wire: a valid request is served with a piece reply") {
    pw_t pw(1, 1, 100, {true}); // we have the only piece
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);
    pw.external_transition(0.0, choke_cmd_box(7, /*unchoke=*/true));
    drain_wire(pw);

    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{request{0, 0, 100}}}));
    auto sent = drain_wire(pw);
    REQUIRE(sent.size() == 1);
    REQUIRE(std::holds_alternative<bep3_msg>(sent[0].second));
    const auto &p = std::get<piece>(std::get<bep3_msg>(sent[0].second));
    CHECK(p.index == 0);
    CHECK(p.begin == 0);
}

TEST_CASE("peer_wire: serving a request emits obs_upload with the served byte count") {
    pw_t pw(1, 1, 100, {true}); // we have the only piece
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);
    pw.external_transition(0.0, choke_cmd_box(7, /*unchoke=*/true));
    drain_wire(pw);

    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{request{0, 0, 100}}}));
    auto obs = drain_obs(pw);
    REQUIRE(obs.size() == 1);
    const auto *upload = std::get_if<bt_utp::obs_upload>(&obs[0]);
    REQUIRE(upload != nullptr);
    CHECK(upload->peer == 7);
    CHECK(upload->piece_index == 0);
    CHECK(upload->bytes == 100);
}

TEST_CASE("peer_wire: sub-piece completion assembles into a full piece and emits have") {
    pw_t pw(1, 2, 100, {false}, /*connect_to=*/7); // 1 piece, 2 sub-pieces of 100B each
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);
    pw.external_transition(0.0, plan_box(7, {{0, 0}, {0, 1}}));
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{unchoke{}}}));
    REQUIRE(drain_wire(pw).size() == 2); // both sub-pieces requested

    // First sub-piece: partial completion only.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{piece{0, 0, 100}}}));
    CHECK(drain_wire(pw).empty()); // no `have` yet, piece not fully assembled

    // Second sub-piece: completes the piece.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{piece{0, 100, 100}}}));
    auto sent = drain_wire(pw);
    REQUIRE(sent.size() == 1);
    REQUIRE(std::holds_alternative<bep3_msg>(sent[0].second));
    CHECK(std::holds_alternative<have>(std::get<bep3_msg>(sent[0].second)));
}

TEST_CASE("peer_wire: unsolicited piece data is rejected") {
    pw_t pw(1, 1, 100, {false}, /*connect_to=*/7);
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);

    // No request was ever made for this sub-piece.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{piece{0, 0, 100}}}));
    CHECK(drain_wire(pw).empty());
}

TEST_CASE("peer_wire: an out-of-range sub-piece offset in a piece message is rejected") {
    pw_t pw(1, 2, 100, {false}, /*connect_to=*/7);
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);
    pw.external_transition(0.0, plan_box(7, {{0, 0}}));
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{unchoke{}}}));
    drain_wire(pw);

    // begin=300 with sub_piece_bytes=100 -> sub_index=3, out of the
    // 2-sub-pieces-per-piece range configured above.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{piece{0, 300, 100}}}));
    CHECK(drain_wire(pw).empty());
}

TEST_CASE("peer_wire: a piece message whose begin isn't a sub-piece boundary is rejected") {
    // Plan *both* sub-pieces of the one piece so sub-piece 1 (the id
    // begin=150 maps onto via integer division) is genuinely outstanding
    // — otherwise the outstanding-match guard alone would reject this
    // message regardless of the alignment check, and the test wouldn't
    // actually isolate what it claims to test.
    pw_t pw(1, 2, 100, {false}, /*connect_to=*/7);
    drain_wire(pw);
    pw.external_transition(0.0, wire_in_box(7, wire_msg{handshake{}}));
    drain_wire(pw);
    pw.external_transition(0.0, plan_box(7, {{0, 0}, {0, 1}}));
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{unchoke{}}}));
    REQUIRE(drain_wire(pw).size() == 2); // both sub-pieces requested

    // Legitimate delivery of sub-piece 0 first (partial completion only).
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{piece{0, 0, 100}}}));
    CHECK(drain_wire(pw).empty());

    // begin=150 is in-range (150/100 == 1 < 2 sub-pieces) and sub-piece 1
    // *is* outstanding, but 150 isn't a multiple of sub_piece_bytes=100 —
    // without the alignment check, integer division would accept this as
    // sub-piece 1, complete the piece, and emit `have`.
    pw.external_transition(0.0, wire_in_box(7, wire_msg{bep3_msg{piece{0, 150, 100}}}));
    CHECK(drain_wire(pw).empty());
}
