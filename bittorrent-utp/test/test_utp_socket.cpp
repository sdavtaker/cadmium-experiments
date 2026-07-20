// SPDX-License-Identifier: BSD-2-Clause
/**
 * Unit tests for the uTP socket atomic: BEP 29 handshake, reliable in-order
 * multi-packet delivery, LEDBAT window behavior, dup-ACK and selective-ACK
 * retransmission, RTO with doubling, 16-bit sequence wraparound, and
 * FIN/RESET teardown — all driven deterministically by stepping two socket
 * instances by hand (no channel, zero latency).
 */
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../models/utp/utp_socket.hpp"
#include "../msg/app_chunk.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdint>
#include <limits>
#include <sstream>
#include <vector>

namespace {

    using bt_utp::app_chunk;
    using bt_utp::packet_type;
    using bt_utp::peer_id;
    using bt_utp::utp_constants;

    using sock_t  = bt_utp::utp_socket<double, app_chunk>;
    using sdefs   = bt_utp::utp_socket_defs_t<app_chunk>;
    using frame_t = sdefs::frame_t;

    using in_box_t = cadmium::make_message_box<sock_t::input_ports>::type;

    in_box_t frame_box(const frame_t &f) {
        in_box_t box;
        cadmium::get_message<sdefs::net_in>(box).emplace(f);
        return box;
    }

    in_box_t send_box(peer_id dst, app_chunk chunk) {
        in_box_t box;
        cadmium::get_message<sdefs::app_send>(box).emplace(sdefs::send_req{dst, chunk});
        return box;
    }

    /// Two directly-wired sockets stepped until quiescent (zero latency).
    struct pair_harness {
        sock_t a;
        sock_t b;
        std::vector<sdefs::deliver_ind> delivered_a{};
        std::vector<sdefs::deliver_ind> delivered_b{};
        std::vector<frame_t> dropped{}; // frames eaten by drop_next
        int drop_next = 0;              // drop the next N frames in transit

        pair_harness(utp_constants ka, utp_constants kb) : a(1, ka), b(2, kb) {}

        bool step_one(sock_t &s, sock_t &peer, std::vector<sdefs::deliver_ind> &delivered) {
            if (s.time_advance() > 0.0) {
                return false;
            }
            auto out = s.output();
            s.internal_transition();
            if (const auto &d = cadmium::get_message<sdefs::app_deliver>(out); d.has_value()) {
                delivered.push_back(*d);
            }
            if (const auto &f = cadmium::get_message<sdefs::net_out>(out); f.has_value()) {
                if (drop_next > 0) {
                    --drop_next;
                    dropped.push_back(*f);
                } else {
                    peer.external_transition(0.0, frame_box(*f));
                }
            }
            return true;
        }

        void pump(int max_steps = 100000) {
            for (int i = 0; i < max_steps; ++i) {
                const bool moved = step_one(a, b, delivered_a) || step_one(b, a, delivered_b);
                if (!moved) {
                    return;
                }
            }
            FAIL("pump did not quiesce");
        }

        void connect() {
            a.external_transition(0.0, send_box(2, app_chunk{0, 1}));
            pump();
        }
    };

    utp_constants big_window(std::uint64_t start_bytes) {
        utp_constants k{};
        k.initial_cwnd = start_bytes; // large starting window for multi-inflight tests
        return k;
    }

} // namespace

TEST_CASE("utp_socket: handshake connects both endpoints and delivers first chunk") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();

    REQUIRE(h.a.connection(2) != nullptr);
    REQUIRE(h.b.connection(1) != nullptr);
    CHECK(h.a.connection(2)->state == sock_t::conn_state::connected);
    CHECK(h.b.connection(1)->state == sock_t::conn_state::connected);
    // conn id semantics: initiator recv id + 1 = send id; acceptor mirrors.
    CHECK(h.a.connection(2)->conn_id_send == h.b.connection(1)->conn_id_recv);
    CHECK(h.b.connection(1)->conn_id_send == h.a.connection(2)->conn_id_recv);
    REQUIRE(h.delivered_b.size() == 1);
    CHECK(h.delivered_b[0].payload == app_chunk{0, 1});
}

TEST_CASE("utp_socket: multi-packet chunk delivered whole, in order, exactly once") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();

    h.a.external_transition(0.0, send_box(2, app_chunk{1, 5000}));
    h.a.external_transition(0.0, send_box(2, app_chunk{2, 300}));
    h.pump();

    REQUIRE(h.delivered_b.size() == 3); // connect chunk + 2
    CHECK(h.delivered_b[1].payload == app_chunk{1, 5000});
    CHECK(h.delivered_b[2].payload == app_chunk{2, 300});
    CHECK(h.a.connection(2)->inflight.empty());
    CHECK(h.a.state.retransmits == 0);
}

TEST_CASE("utp_socket: slow start grows cwnd on below-target acks") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();
    const double cwnd0 = h.a.connection(2)->cwnd;

    h.a.external_transition(0.0, send_box(2, app_chunk{1, 5000}));
    h.pump();

    CHECK(h.a.connection(2)->slow_start);
    CHECK(h.a.connection(2)->cwnd > cwnd0);
}

TEST_CASE("utp_socket: LEDBAT backs off when measured delay exceeds target") {
    utp_constants k = big_window(4000);
    pair_harness h{k, k};
    h.connect();

    // Round 1: send data, ack with a small delay sample (establishes base).
    h.a.external_transition(0.0, send_box(2, app_chunk{1, 900}));
    auto first = h.a.output();
    h.a.internal_transition();
    const auto sent1 = *cadmium::get_message<sdefs::net_out>(first);

    frame_t ack1{};
    ack1.src                                   = 2;
    ack1.dst                                   = 1;
    ack1.pkt.type                              = packet_type::st_state;
    ack1.pkt.connection_id                     = h.a.connection(2)->conn_id_recv;
    ack1.pkt.ack_nr                            = sent1.pkt.seq_nr;
    ack1.pkt.seq_nr                            = h.b.connection(1)->seq_nr;
    ack1.pkt.wnd_size                          = 1 << 20;
    ack1.pkt.timestamp_difference_microseconds = 10000; // 10 ms
    h.a.external_transition(0.0, frame_box(ack1));

    // Round 2: another packet, acked with 260 ms sample -> our_delay 250 ms
    // above the 100 ms target: slow start must end and cwnd must shrink.
    h.a.external_transition(0.0, send_box(2, app_chunk{2, 900}));
    auto second = h.a.output();
    h.a.internal_transition();
    const auto sent2      = *cadmium::get_message<sdefs::net_out>(second);
    const double cwnd_pre = h.a.connection(2)->cwnd;

    frame_t ack2                               = ack1;
    ack2.pkt.ack_nr                            = sent2.pkt.seq_nr;
    ack2.pkt.timestamp_difference_microseconds = 260000;
    h.a.external_transition(0.0, frame_box(ack2));

    CHECK_FALSE(h.a.connection(2)->slow_start);
    CHECK(h.a.connection(2)->cwnd < cwnd_pre);
}

TEST_CASE("utp_socket: three duplicate acks trigger fast retransmit and window cut") {
    utp_constants k = big_window(4000);
    pair_harness h{k, k};
    h.connect();

    // Put a packet in flight without delivering it.
    h.a.external_transition(0.0, send_box(2, app_chunk{1, 900}));
    auto out = h.a.output();
    h.a.internal_transition();
    const auto lost = *cadmium::get_message<sdefs::net_out>(out);
    REQUIRE(h.a.connection(2)->inflight.size() == 1);
    const double cwnd_pre = h.a.connection(2)->cwnd;

    frame_t dup{};
    dup.src               = 2;
    dup.dst               = 1;
    dup.pkt.type          = packet_type::st_state;
    dup.pkt.connection_id = h.a.connection(2)->conn_id_recv;
    dup.pkt.seq_nr        = h.b.connection(1)->seq_nr;
    dup.pkt.wnd_size      = 1 << 20;
    dup.pkt.ack_nr        = static_cast<std::uint16_t>(lost.pkt.seq_nr - 1); // acks nothing
    for (int i = 0; i < 4; ++i) { // 1 establishes last_ack_seen + 3 duplicates
        h.a.external_transition(0.0, frame_box(dup));
    }

    CHECK(h.a.state.retransmits == 1);
    CHECK(h.a.connection(2)->cwnd < cwnd_pre);
}

TEST_CASE("utp_socket: selective-ack holes retransmit after three packets acked past") {
    utp_constants k = big_window(8000);
    pair_harness h{k, k};
    h.connect();

    // 4 packets in flight, none delivered.
    for (int i = 1; i <= 4; ++i) {
        h.a.external_transition(0.0, send_box(2, app_chunk{static_cast<std::uint64_t>(i), 900}));
    }
    std::vector<frame_t> sent;
    while (h.a.time_advance() == 0.0) {
        auto out = h.a.output();
        h.a.internal_transition();
        if (const auto &f = cadmium::get_message<sdefs::net_out>(out); f.has_value()) {
            sent.push_back(*f);
        }
    }
    REQUIRE(sent.size() == 4);
    const std::uint16_t first_seq = sent[0].pkt.seq_nr;

    // SACK acking packets first+1..first+3 (bits 0..2 relative ack_nr+2),
    // leaving the first as a hole with 3 packets acked past it.
    frame_t sack{};
    sack.src               = 2;
    sack.dst               = 1;
    sack.pkt.type          = packet_type::st_state;
    sack.pkt.connection_id = h.a.connection(2)->conn_id_recv;
    sack.pkt.seq_nr        = h.b.connection(1)->seq_nr;
    sack.pkt.wnd_size      = 1 << 20;
    sack.pkt.ack_nr        = static_cast<std::uint16_t>(first_seq - 1);
    sack.pkt.sack_mask     = {0b00000111, 0, 0, 0};
    const double cwnd_pre  = h.a.connection(2)->cwnd;
    h.a.external_transition(0.0, frame_box(sack));

    CHECK(h.a.state.retransmits == 1);
    CHECK(h.a.connection(2)->inflight.size() == 1); // only the hole remains
    CHECK(h.a.connection(2)->inflight.front().seq == first_seq);
    CHECK(h.a.connection(2)->cwnd < cwnd_pre);
}

TEST_CASE("utp_socket: retransmission timeout doubles timeout and floors the window") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();

    // Send one packet and lose it: RTO must fire after the initial timeout.
    h.drop_next = 1;
    h.a.external_transition(0.0, send_box(2, app_chunk{1, 900}));
    h.pump();
    REQUIRE(h.dropped.size() == 1);
    REQUIRE(h.a.connection(2)->inflight.size() == 1);

    const double rto = h.a.time_advance();
    CHECK(rto > 0.0);
    h.a.internal_transition(); // fire the timeout: queues the retransmission

    CHECK(h.a.state.timeouts == 1);
    CHECK(h.a.connection(2)->timeout > rto); // doubled
    CHECK(h.a.connection(2)->cwnd == 150.0); // floored to min packet

    h.pump(); // retransmitted frame reaches B now
    REQUIRE(h.delivered_b.size() == 2);
    CHECK(h.delivered_b[1].payload == app_chunk{1, 900});
    CHECK(h.a.connection(2)->inflight.empty());
}

TEST_CASE("utp_socket: 16-bit sequence numbers wrap without reordering") {
    utp_constants kb = utp_constants{};
    kb.acceptor_seq0 = 65533; // acceptor's stream crosses the wrap
    pair_harness h{utp_constants{}, kb};
    h.connect();

    for (std::uint64_t i = 1; i <= 8; ++i) {
        h.b.external_transition(0.0, send_box(1, app_chunk{i, 400}));
    }
    h.pump();

    REQUIRE(h.delivered_a.size() == 8);
    for (std::uint64_t i = 1; i <= 8; ++i) {
        CHECK(h.delivered_a[i - 1].payload == app_chunk{i, 400});
    }
    // The acceptor's seq counter indeed wrapped below its start value.
    CHECK(h.b.connection(1)->seq_nr < 65533);
}

TEST_CASE("utp_socket: FIN closes once every prior packet arrived") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();

    frame_t fin{};
    fin.src               = 2;
    fin.dst               = 1;
    fin.pkt.type          = packet_type::st_fin;
    fin.pkt.connection_id = h.a.connection(2)->conn_id_recv;
    fin.pkt.seq_nr        = h.b.connection(1)->seq_nr; // next in order
    fin.pkt.wnd_size      = 1 << 20;
    fin.pkt.ack_nr        = h.a.connection(2)->seq_nr == 0
                                ? std::uint16_t{0}
                                : static_cast<std::uint16_t>(h.a.connection(2)->seq_nr - 1);
    h.a.external_transition(0.0, frame_box(fin));

    CHECK(h.a.connection(2)->state == sock_t::conn_state::closed);
}

TEST_CASE("utp_socket: packets for unknown connections draw ST_RESET; reset kills the conn") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();

    // Unknown source peer -> socket answers with ST_RESET.
    frame_t stray{};
    stray.src        = 99;
    stray.dst        = 1;
    stray.pkt.type   = packet_type::st_data;
    stray.pkt.seq_nr = 7;
    h.a.external_transition(0.0, frame_box(stray));
    auto out = h.a.output();
    h.a.internal_transition();
    const auto &rst = cadmium::get_message<sdefs::net_out>(out);
    REQUIRE(rst.has_value());
    CHECK(rst->pkt.type == packet_type::st_reset);
    CHECK(rst->dst == 99);

    // Receiving ST_RESET on a live connection (correct connection_id) closes it.
    frame_t reset{};
    reset.src               = 2;
    reset.dst               = 1;
    reset.pkt.type          = packet_type::st_reset;
    reset.pkt.connection_id = h.a.connection(2)->conn_id_recv;
    h.a.external_transition(0.0, frame_box(reset));
    CHECK(h.a.connection(2)->state == sock_t::conn_state::closed);
}

TEST_CASE("utp_socket: a packet with the wrong connection_id is rejected, not accepted") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();

    const std::uint16_t expected_id = h.a.connection(2)->conn_id_recv;
    const std::uint16_t stale_ack   = h.a.connection(2)->ack_nr;

    // Known peer, but a connection_id that doesn't match what this socket
    // expects to receive on: BEP 29 uses connection_id to disambiguate
    // streams between the same pair of endpoints, so this must be treated
    // as foreign traffic (draws ST_RESET), not folded into the live conn.
    frame_t foreign{};
    foreign.src               = 2;
    foreign.dst               = 1;
    foreign.pkt.type          = packet_type::st_data;
    foreign.pkt.connection_id = static_cast<std::uint16_t>(expected_id + 1);
    foreign.pkt.seq_nr        = static_cast<std::uint16_t>(stale_ack + 1);
    foreign.pkt.wnd_size      = 1 << 20;
    h.a.external_transition(0.0, frame_box(foreign));

    auto out = h.a.output();
    h.a.internal_transition();
    const auto &rst = cadmium::get_message<sdefs::net_out>(out);
    REQUIRE(rst.has_value());
    CHECK(rst->pkt.type == packet_type::st_reset);

    // The live connection is untouched: still connected, ack_nr unmoved.
    CHECK(h.a.connection(2)->state == sock_t::conn_state::connected);
    CHECK(h.a.connection(2)->ack_nr == stale_ack);
}

TEST_CASE("utp_socket: app_send reopens a fresh connection instead of dropping into a closed one") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();

    const std::uint16_t old_conn_id = h.a.connection(2)->conn_id_recv;

    frame_t reset{};
    reset.src               = 2;
    reset.dst               = 1;
    reset.pkt.type          = packet_type::st_reset;
    reset.pkt.connection_id = old_conn_id;
    h.a.external_transition(0.0, frame_box(reset));
    REQUIRE(h.a.connection(2)->state == sock_t::conn_state::closed);

    // A send to the same peer after close must not enqueue into the dead
    // entry (packetize() only ever runs while CONNECTED, so a payload
    // parked there would sit forever) — it must open a fresh connection.
    h.a.external_transition(0.0, send_box(2, app_chunk{9, 500}));
    const auto *fresh = h.a.connection(2);
    REQUIRE(fresh != nullptr);
    CHECK(fresh->state == sock_t::conn_state::syn_sent);
    CHECK(fresh->conn_id_recv != old_conn_id);
    CHECK(fresh->pending.size() == 1);
    CHECK(fresh->pending.front().payload == app_chunk{9, 500});
}

TEST_CASE("utp_socket: state streams to log-friendly text") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();
    std::ostringstream os;
    os << h.a.state;
    CHECK(os.str().find("CONNECTED") != std::string::npos);
    CHECK(os.str().find("cwnd:") != std::string::npos);
}
