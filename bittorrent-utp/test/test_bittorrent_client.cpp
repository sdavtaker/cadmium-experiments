// SPDX-License-Identifier: BSD-2-Clause
/**
 * Structural test for bittorrent_client: verifies the coupled-model wiring
 * (socket + peer_wire + stub policies) instantiates through a real
 * coordinator and schedules without crashing.
 *
 * This is intentionally not a behavioral end-to-end test: utp_socket needs
 * an actual peer on the other end of a channel to complete its own uTP
 * handshake before peer_wire's BEP 3 handshake can even be transmitted —
 * that requires two wired clients, exercised by a later deterministic
 * two-client end-to-end test, not this structural one.
 */
#include <cadmium/engine/devs_coordinator.hpp>

#include "../models/client/bittorrent_client.hpp"
#include "../models/client/peer_wire.hpp"
#include "../models/client/stub_always_unchoke.hpp"
#include "../models/client/stub_sequential_selector.hpp"
#include "../models/utp/utp_socket.hpp"
#include "../msg/peer_id.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

    constexpr bt_utp::peer_id client_a_id        = 1;
    constexpr bt_utp::peer_id client_b_id        = 2;
    constexpr std::uint32_t total_pieces         = 4;
    constexpr std::uint32_t sub_pieces_per_piece = 2;
    constexpr std::uint32_t sub_piece_bytes      = 100;

    template <typename TIME>
    struct client_a_socket : public bt_utp::utp_socket<TIME, bt_utp::wire_msg> {
        client_a_socket()
            : bt_utp::utp_socket<TIME, bt_utp::wire_msg>(client_a_id, bt_utp::utp_constants{}) {}
    };
    template <typename TIME> struct client_a_wire : public bt_utp::peer_wire<TIME> {
        client_a_wire()
            : bt_utp::peer_wire<TIME>(total_pieces, sub_pieces_per_piece, sub_piece_bytes,
                                      std::vector<bool>(total_pieces, true), client_b_id) {}
    };
    template <typename TIME>
    struct client_a_selector : public bt_utp::stub_sequential_selector<TIME> {
        client_a_selector() : bt_utp::stub_sequential_selector<TIME>(sub_pieces_per_piece) {}
    };

    template <typename TIME>
    using client_a = bt_utp::bittorrent_client<TIME, client_a_socket, client_a_wire,
                                               bt_utp::stub_always_unchoke, client_a_selector>;

} // namespace

TEST_CASE("bittorrent_client: coupling constructs and schedules through a real coordinator") {
    cadmium::engine::devs::coordinator<client_a, double> coord;
    coord.init(0.0);

    // client_a_wire actively opens toward client_b_id, so peer_wire has a
    // t=0 handshake+bitfield queued from construction; the coupled model
    // as a whole must therefore be imminent at t=0, not passive forever.
    const double next = coord.next();
    CHECK(next < std::numeric_limits<double>::infinity());

    coord.collect_outputs(next);
    coord.advance_simulation(next);
    CHECK(coord.next() >= next); // still a well-formed schedule after one step
}
