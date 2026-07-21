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
#include <stdexcept>
#include <string>
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

    in_box_t frame_and_send_box(const frame_t &f, peer_id dst, app_chunk chunk) {
        in_box_t box;
        cadmium::get_message<sdefs::net_in>(box).emplace(f);
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

TEST_CASE("utp_socket: output()/internal_transition() reject being called while passive") {
    sock_t sock(1, utp_constants{}); // no connections, no timers armed: passive
    REQUIRE(sock.time_advance() == std::numeric_limits<double>::infinity());
    CHECK_THROWS_AS(sock.output(), std::logic_error);
    CHECK_THROWS_AS(sock.internal_transition(), std::logic_error);
}

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

TEST_CASE("utp_socket: dup-ack counter resets when ack_nr changes without acking new data") {
    utp_constants k = big_window(4000);
    pair_harness h{k, k};
    h.connect();

    h.a.external_transition(0.0, send_box(2, app_chunk{1, 900}));
    auto out = h.a.output();
    h.a.internal_transition();
    const auto lost = *cadmium::get_message<sdefs::net_out>(out);
    REQUIRE(h.a.connection(2)->inflight.size() == 1);

    auto ack_with = [&](std::uint16_t ack_nr) {
        frame_t f{};
        f.src               = 2;
        f.dst               = 1;
        f.pkt.type          = packet_type::st_state;
        f.pkt.connection_id = h.a.connection(2)->conn_id_recv;
        f.pkt.seq_nr        = h.b.connection(1)->seq_nr;
        f.pkt.wnd_size      = 1 << 20;
        f.pkt.ack_nr        = ack_nr;
        return f;
    };
    // Both values ack nothing (below the single inflight packet's seq) and
    // deliberately avoid connect()'s own last_ack_seen baseline: its DATA
    // exchange leaves last_ack_seen at seq 2 (SYN=1, that DATA=2), and this
    // test's packet is seq 3 — reusing "seq - 1" (= 2) as the first probed
    // value would collide with that baseline and start counting duplicates
    // a step early.
    REQUIRE(h.a.connection(2)->last_ack_seen == static_cast<std::uint16_t>(lost.pkt.seq_nr - 1));
    const std::uint16_t below = 1;
    const std::uint16_t other = 0;

    h.a.external_transition(0.0, frame_box(ack_with(below))); // establishes last_ack_seen
    h.a.external_transition(0.0, frame_box(ack_with(below))); // dup 1
    h.a.external_transition(0.0, frame_box(ack_with(below))); // dup 2
    REQUIRE(h.a.connection(2)->dup_acks == 2);
    REQUIRE(h.a.state.retransmits == 0); // threshold (3) not yet reached

    // A different ack_nr appears once (e.g. a reordered/stale ack): must not
    // count as a duplicate of anything, and must clear the stale counter.
    h.a.external_transition(0.0, frame_box(ack_with(other)));
    CHECK(h.a.connection(2)->dup_acks == 0);

    // Two more occurrences of the ORIGINAL value must resume counting from
    // zero, not silently reuse the pre-reset count and fire early.
    h.a.external_transition(0.0, frame_box(ack_with(below)));
    h.a.external_transition(0.0, frame_box(ack_with(below)));
    CHECK(h.a.connection(2)->dup_acks == 1);
    CHECK(h.a.state.retransmits == 0);
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

TEST_CASE("utp_socket: reply_micro updates from ST_STATE packets, not just data") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();

    frame_t ack{};
    ack.src                        = 2;
    ack.dst                        = 1;
    ack.pkt.type                   = packet_type::st_state;
    ack.pkt.connection_id          = h.a.connection(2)->conn_id_recv;
    ack.pkt.seq_nr                 = h.b.connection(1)->seq_nr;
    ack.pkt.ack_nr                 = h.a.connection(2)->seq_nr;
    ack.pkt.wnd_size               = 1 << 20;
    ack.pkt.timestamp_microseconds = 500;
    h.a.external_transition(1.0, frame_box(ack)); // advance a's clock to 1s
    CHECK(h.a.connection(2)->reply_micro == 1'000'000u - 500u);
}

TEST_CASE("utp_socket: a RESET echoing our conn_id_send (a peer's reject-style reset) closes "
          "the connection") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();

    // Mirrors how this socket itself builds a reject reset for foreign
    // traffic: connection_id = the value seen on the packet that confused
    // the sender, i.e. OUR conn_id_send (not our conn_id_recv).
    frame_t reset{};
    reset.src               = 2;
    reset.dst               = 1;
    reset.pkt.type          = packet_type::st_reset;
    reset.pkt.connection_id = h.a.connection(2)->conn_id_send;
    h.a.external_transition(0.0, frame_box(reset));

    CHECK(h.a.connection(2)->state == sock_t::conn_state::closed);
}

TEST_CASE("utp_socket: acceptor treats a SYN with a changed connection_id as a fresh stream") {
    pair_harness h{utp_constants{}, utp_constants{}};

    frame_t syn1{};
    syn1.src               = 1;
    syn1.dst               = 2;
    syn1.pkt.type          = packet_type::st_syn;
    syn1.pkt.connection_id = 100;
    syn1.pkt.seq_nr        = 1;
    syn1.pkt.wnd_size      = 1 << 20;
    h.b.external_transition(0.0, frame_box(syn1));
    REQUIRE(h.b.connection(1) != nullptr);
    CHECK(h.b.connection(1)->conn_id_send == 100);
    CHECK(h.b.connection(1)->conn_id_recv == 101);

    // Same peer restarts the stream with a different connection_id before
    // the old entry closed: must not keep the stale ids.
    frame_t syn2           = syn1;
    syn2.pkt.connection_id = 200;
    h.b.external_transition(0.0, frame_box(syn2));
    REQUIRE(h.b.connection(1) != nullptr);
    CHECK(h.b.connection(1)->state == sock_t::conn_state::syn_recv);
    CHECK(h.b.connection(1)->conn_id_send == 200);
    CHECK(h.b.connection(1)->conn_id_recv == 201);
}

TEST_CASE("utp_socket: acceptor flushes payload queued during SYN_RECV once connected") {
    pair_harness h{utp_constants{}, utp_constants{}};

    frame_t syn{};
    syn.src               = 1;
    syn.dst               = 2;
    syn.pkt.type          = packet_type::st_syn;
    syn.pkt.connection_id = 42;
    syn.pkt.seq_nr        = 1;
    syn.pkt.wnd_size      = 1 << 20;
    h.b.external_transition(0.0, frame_box(syn));
    REQUIRE(h.b.connection(1) != nullptr);
    REQUIRE(h.b.connection(1)->state == sock_t::conn_state::syn_recv);

    // The acceptor's app layer queues a send before the initiator's first
    // ST_DATA has arrived — handle_app_send defers it since the connection
    // isn't CONNECTED yet.
    h.b.external_transition(0.0, send_box(1, app_chunk{5, 400}));
    REQUIRE(h.b.connection(1)->pending.size() == 1);
    CHECK(h.b.connection(1)->inflight.empty());

    // Initiator's first ST_DATA arrives, completing the handshake: the
    // deferred payload must be flushed now, not stuck forever.
    frame_t data{};
    data.src               = 1;
    data.dst               = 2;
    data.pkt.type          = packet_type::st_data;
    data.pkt.connection_id = h.b.connection(1)->conn_id_recv;
    data.pkt.seq_nr        = static_cast<std::uint16_t>(h.b.connection(1)->ack_nr + 1);
    data.pkt.ack_nr        = h.b.connection(1)->seq_nr;
    data.pkt.wnd_size      = 1 << 20;
    data.pkt.payload_size  = 10;
    h.b.external_transition(0.0, frame_box(data));

    CHECK(h.b.connection(1)->state == sock_t::conn_state::connected);
    CHECK(h.b.connection(1)->pending.empty());
    REQUIRE(h.b.connection(1)->inflight.size() == 1);
    CHECK(h.b.connection(1)->inflight.front().completing == std::vector<app_chunk>{{5, 400}});
}

TEST_CASE("utp_socket: simultaneous open — inbound SYN and app_send to the same peer in one "
          "instant") {
    // Both ports carry a message for the SAME peer in a single
    // external_transition (simultaneous open): a real inbound SYN from
    // peer 2, and an app_send targeting peer 2. net_in must be processed
    // first so app_send finds and queues into the acceptor connection
    // accept_syn() just created, instead of first opening its own
    // initiator connection (queuing an outgoing SYN) that accept_syn then
    // erases/replaces, orphaning that queued SYN.
    pair_harness h{utp_constants{}, utp_constants{}};

    frame_t syn{};
    syn.src               = 2;
    syn.dst               = 1;
    syn.pkt.type          = packet_type::st_syn;
    syn.pkt.connection_id = 77;
    syn.pkt.seq_nr        = 1;
    syn.pkt.wnd_size      = 1 << 20;

    h.a.external_transition(0.0, frame_and_send_box(syn, 2, app_chunk{3, 200}));

    REQUIRE(h.a.connection(2) != nullptr);
    const auto *c = h.a.connection(2);
    CHECK(c->state == sock_t::conn_state::syn_recv); // acceptor role, not initiator
    CHECK(c->conn_id_send == 77);                    // ids derived from the real inbound SYN
    CHECK(c->conn_id_recv == 78);
    CHECK(c->pending.size() == 1); // the app_send was queued into this connection

    // Only the SYN's ST_STATE ack is pending output — no orphaned outgoing
    // SYN from a since-erased initiator attempt.
    auto out = h.a.output();
    h.a.internal_transition();
    const auto &sent = cadmium::get_message<sdefs::net_out>(out);
    REQUIRE(sent.has_value());
    CHECK(sent->pkt.type == packet_type::st_state);
    CHECK(h.a.time_advance() == std::numeric_limits<double>::infinity());
}

TEST_CASE("utp_socket: state streams to log-friendly text") {
    pair_harness h{utp_constants{}, utp_constants{}};
    h.connect();
    std::ostringstream os;
    os << h.a.state;
    CHECK(os.str().find("CONNECTED") != std::string::npos);
    CHECK(os.str().find("cwnd:") != std::string::npos);
}
