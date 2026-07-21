// SPDX-License-Identifier: BSD-2-Clause
/**
 * Unit tests for the BEP 3 message set (msg/bep3.hpp): wire_size accounting
 * for every message type, variant round-trip (construct -> hold in
 * bep3_msg -> extract), and streaming.
 */
#include "../msg/bep3.hpp"
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <variant>

namespace {

    using bt_utp::bep3_msg;
    using bt_utp::bitfield;
    using bt_utp::cancel;
    using bt_utp::choke;
    using bt_utp::handshake;
    using bt_utp::have;
    using bt_utp::interested;
    using bt_utp::keepalive;
    using bt_utp::not_interested;
    using bt_utp::piece;
    using bt_utp::request;
    using bt_utp::unchoke;
    using bt_utp::wire_size;

} // namespace

TEST_CASE("bep3: handshake wire size is pstrlen + fixed fields") {
    handshake h{};
    // 1 (pstrlen byte) + 19 (pstr) + 8 (reserved) + 20 (info_hash) + 20 (peer_id) = 68
    CHECK(h.wire_size() == 68);
}

TEST_CASE("bep3: keepalive wire size is the length prefix alone, no id byte") {
    CHECK(keepalive{}.wire_size() == 4);
}

TEST_CASE("bep3: flag message wire sizes are length-prefix + id, no payload") {
    CHECK(choke{}.wire_size() == 5);
    CHECK(unchoke{}.wire_size() == 5);
    CHECK(interested{}.wire_size() == 5);
    CHECK(not_interested{}.wire_size() == 5);
}

TEST_CASE("bep3: have/request/cancel wire sizes include their fixed payload") {
    CHECK(have{7}.wire_size() == 5 + 4);
    CHECK(request{1, 0, 16384}.wire_size() == 5 + 12);
    CHECK(cancel{1, 0, 16384}.wire_size() == 5 + 12);
}

TEST_CASE("bep3: piece wire size includes its abstracted payload_size") {
    piece p{2, 16384, 16384};
    CHECK(p.wire_size() == 5 + 8 + 16384);
}

TEST_CASE("bep3: bitfield wire size is ceil(num_pieces/8) bytes") {
    CHECK(bitfield{std::vector<bool>(40, false)}.wire_size() == 5 + 5); // 40/8 = 5
    CHECK(bitfield{std::vector<bool>(41, false)}.wire_size() == 5 + 6); // ceil(41/8) = 6
    CHECK(bitfield{std::vector<bool>{}}.wire_size() == 5);              // 0 pieces
}

TEST_CASE("bep3_msg variant: each alternative round-trips and dispatches wire_size") {
    bep3_msg m = have{3};
    REQUIRE(std::holds_alternative<have>(m));
    CHECK(std::get<have>(m).index == 3);
    CHECK(wire_size(m) == have{3}.wire_size());

    m = piece{1, 0, 1000};
    REQUIRE(std::holds_alternative<piece>(m));
    CHECK(wire_size(m) == piece{1, 0, 1000}.wire_size());

    m = choke{};
    REQUIRE(std::holds_alternative<choke>(m));
    CHECK(wire_size(m) == choke{}.wire_size());
}

TEST_CASE("bep3: messages stream to non-empty human-readable text") {
    std::ostringstream os;
    os << handshake{} << " " << bep3_msg{have{5}} << " " << bep3_msg{request{1, 0, 100}};
    const std::string s = os.str();
    CHECK(s.find("HANDSHAKE") != std::string::npos);
    CHECK(s.find("HAVE idx:5") != std::string::npos);
    CHECK(s.find("REQUEST idx:1") != std::string::npos);
}

TEST_CASE("bep3: equality comparisons hold field-by-field") {
    CHECK(have{1} == have{1});
    CHECK_FALSE(have{1} == have{2});
    CHECK(request{1, 2, 3} == request{1, 2, 3});
    CHECK_FALSE(request{1, 2, 3} == request{1, 2, 4});
    CHECK(bitfield{{true, false}} == bitfield{{true, false}});
    CHECK_FALSE(bitfield{{true, false}} == bitfield{{true, true}});
}
