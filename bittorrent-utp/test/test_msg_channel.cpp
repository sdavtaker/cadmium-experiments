// SPDX-License-Identifier: BSD-2-Clause
/**
 * Unit tests for the uTP message types and the deterministic bottleneck
 * channel, including the classic-DEVS simultaneity micro-test (two sources
 * emitting into one channel port at the same instant through the
 * coordinator — fable_plan.md 8.2).
 */
#include <cadmium/engine/devs_coordinator.hpp>
#include <cadmium/engine/devs_engine_helpers.hpp>
#include <cadmium/modeling/coupling.hpp>
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../models/utp/bottleneck_channel.hpp"
#include "../msg/net_frame.hpp"
#include "../msg/utp_frame.hpp" // together with net_frame.hpp: regression test for shared peer_id
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {

    using bt_utp::bottleneck_channel;
    using bt_utp::bottleneck_channel_defs;
    using bt_utp::net_frame;
    using bt_utp::packet_type;
    using bt_utp::utp_packet;

    net_frame make_frame(std::uint16_t seq, std::uint64_t payload) {
        net_frame f{};
        f.src              = 1;
        f.dst              = 2;
        f.pkt.type         = packet_type::st_data;
        f.pkt.seq_nr       = seq;
        f.pkt.payload_size = payload;
        return f;
    }

    using channel_t = bottleneck_channel<double>;

    typename cadmium::make_message_box<channel_t::input_ports>::type box_with(const net_frame &f) {
        typename cadmium::make_message_box<channel_t::input_ports>::type box;
        cadmium::get_message<bottleneck_channel_defs::in>(box).emplace(f);
        return box;
    }

} // namespace

TEST_CASE("utp_packet wire size accounts header, extension, and payload") {
    utp_packet p{};
    CHECK(p.wire_size() == bt_utp::utp_header_bytes);

    p.payload_size = 1000;
    CHECK(p.wire_size() == bt_utp::utp_header_bytes + 1000);

    p.sack_mask = {0x01, 0x00, 0x00, 0x00};
    CHECK(p.wire_size() == bt_utp::utp_header_bytes + 2 + 4 + 1000);

    // Only ST_DATA carries payload (BEP 29); a stray payload_size on a pure
    // ACK must not inflate byte accounting.
    p.type = packet_type::st_state;
    CHECK(p.wire_size() == bt_utp::utp_header_bytes + 2 + 4);
}

TEST_CASE("message types stream to non-empty human-readable text") {
    std::ostringstream os;
    os << make_frame(7, 42);
    CHECK(os.str().find("ST_DATA") != std::string::npos);
    CHECK(os.str().find("seq:7") != std::string::npos);
    CHECK(os.str().find("1->2") != std::string::npos);
}

TEST_CASE("bottleneck channel: analytic FIFO departure and delivery times") {
    // R = 1000 B/s, D = 0.05 s; two 1000 B frames arriving back-to-back at
    // t=0 must exit at t=1.05 and t=2.05 (service 1 s each, FIFO).
    channel_t ch{0.05, 1000.0};

    ch.external_transition(0.0, box_with(make_frame(1, 1000 - bt_utp::utp_header_bytes)));
    REQUIRE_THAT(ch.time_advance(), Catch::Matchers::WithinAbs(1.05, 1e-9));

    ch.external_transition(0.0, box_with(make_frame(2, 1000 - bt_utp::utp_header_bytes)));
    REQUIRE_THAT(ch.time_advance(), Catch::Matchers::WithinAbs(1.05, 1e-9));

    auto out1 = ch.output();
    REQUIRE(cadmium::get_message<bottleneck_channel_defs::out>(out1).has_value());
    CHECK(cadmium::get_message<bottleneck_channel_defs::out>(out1)->pkt.seq_nr == 1);
    ch.internal_transition();

    REQUIRE_THAT(ch.time_advance(), Catch::Matchers::WithinAbs(1.0, 1e-9));
    auto out2 = ch.output();
    REQUIRE(cadmium::get_message<bottleneck_channel_defs::out>(out2).has_value());
    CHECK(cadmium::get_message<bottleneck_channel_defs::out>(out2)->pkt.seq_nr == 2);
    ch.internal_transition();

    CHECK(ch.time_advance() == std::numeric_limits<double>::infinity());
    CHECK(ch.state.forwarded == 2);
}

TEST_CASE("bottleneck channel: queuing delay emerges from the rate cap") {
    // Arrival while the server is busy waits: second frame arrives at
    // t=0.5 mid-service of the first; its delivery is 2.05, not 1.55.
    channel_t ch{0.05, 1000.0};
    ch.external_transition(0.0, box_with(make_frame(1, 1000 - bt_utp::utp_header_bytes)));
    ch.external_transition(0.5, box_with(make_frame(2, 1000 - bt_utp::utp_header_bytes)));
    // 0.55 remaining for frame 1 (aged by 0.5), frame 2 delivers 1 s later.
    REQUIRE_THAT(ch.time_advance(), Catch::Matchers::WithinAbs(0.55, 1e-9));
    ch.internal_transition();
    REQUIRE_THAT(ch.time_advance(), Catch::Matchers::WithinAbs(1.0, 1e-9));
}

TEST_CASE("bottleneck channel: deterministic drop-every-Nth pattern") {
    channel_t ch{0.0, 1e9, 0, 2}; // drop every 2nd arrival
    for (std::uint16_t i = 1; i <= 4; ++i) {
        ch.external_transition(0.0, box_with(make_frame(i, 100)));
    }
    CHECK(ch.state.received == 4);
    CHECK(ch.state.dropped_nth == 2);
    CHECK(ch.state.pending.size() == 2);
}

TEST_CASE("bottleneck channel: finite queue tail-drops on overflow") {
    // Slow server (1 B/s) so nothing clears; capacity 1500 B admits the
    // first 1000 B frame and rejects the second.
    channel_t ch{0.0, 1.0, 1500, 0};
    ch.external_transition(0.0, box_with(make_frame(1, 1000 - bt_utp::utp_header_bytes)));
    ch.external_transition(0.0, box_with(make_frame(2, 1000 - bt_utp::utp_header_bytes)));
    CHECK(ch.state.dropped_overflow == 1);
    CHECK(ch.state.pending.size() == 1);
}

TEST_CASE("bottleneck channel: invalid construction parameters are rejected") {
    CHECK_THROWS_AS(channel_t(0.05, 0.0), std::invalid_argument);
    CHECK_THROWS_AS(channel_t(0.05, -1.0), std::invalid_argument);
    CHECK_THROWS_AS(channel_t(-0.01, 1000.0), std::invalid_argument);
}

TEST_CASE("bottleneck channel: state streams to log-friendly text") {
    channel_t ch{0.05, 1000.0};
    ch.external_transition(0.0, box_with(make_frame(1, 100)));
    std::ostringstream os;
    os << ch.state;
    CHECK(os.str().find("q:1") != std::string::npos);
    CHECK(os.str().find("rx:1") != std::string::npos);
}

TEST_CASE("net_frame and utp_frame share one peer_id definition without conflict") {
    // Compile-time regression: both frame headers are included in this TU
    // (see the includes above); this only builds if their peer_id usages
    // resolve to the single shared alias in peer_id.hpp without collision.
    static_assert(std::is_same_v<decltype(net_frame::src), bt_utp::peer_id>);
    static_assert(std::is_same_v<decltype(bt_utp::utp_frame<int>::src), bt_utp::peer_id>);
    bt_utp::peer_id id = 7;
    net_frame f{};
    f.src = id;
    CHECK(f.src == 7);
}

// ---------------------------------------------------------------------------
// Simultaneity micro-test (fable_plan.md 8.2): two one-shot sources emit into
// the same channel input port at the same simulation instant via the
// coordinator; classic-DEVS SELECT must serialize them into two external
// transitions and both frames must traverse the channel.
// ---------------------------------------------------------------------------

namespace {

    struct shot_a_defs {
        struct out : public cadmium::out_port<net_frame> {};
    };
    struct shot_b_defs {
        struct out : public cadmium::out_port<net_frame> {};
    };

    template <typename DEFS, std::uint16_t SEQ, typename TIME> class one_shot {
      public:
        using state_type = int; // 0 = armed, 1 = done
        state_type state = 0;

        using input_ports  = std::tuple<>;
        using output_ports = std::tuple<typename DEFS::out>;

        void internal_transition() {
            state = 1;
        }
        void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type) {
            throw std::logic_error("one_shot has no inputs");
        }
        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            cadmium::get_message<typename DEFS::out>(box).emplace(make_frame(SEQ, 100));
            return box;
        }
        TIME time_advance() const {
            return state == 0 ? TIME{1} : std::numeric_limits<TIME>::infinity();
        }
    };

    template <typename TIME> using shot_a = one_shot<shot_a_defs, 1, TIME>;
    template <typename TIME> using shot_b = one_shot<shot_b_defs, 2, TIME>;

    template <typename TIME> struct test_channel : public bottleneck_channel<TIME> {
        test_channel() : bottleneck_channel<TIME>(TIME{0.05}, TIME{1000}) {}
    };

    struct top_out : public cadmium::out_port<net_frame> {};

    using empty_iports  = std::tuple<>;
    using empty_eic     = std::tuple<>;
    using top_oports    = std::tuple<top_out>;
    using top_submodels = cadmium::modeling::models_tuple<shot_a, shot_b, test_channel>;
    using top_ic        = std::tuple<
               cadmium::modeling::IC<shot_a, shot_a_defs::out, test_channel, bottleneck_channel_defs::in>,
               cadmium::modeling::IC<shot_b, shot_b_defs::out, test_channel, bottleneck_channel_defs::in>>;
    using top_eoc =
        std::tuple<cadmium::modeling::EOC<test_channel, bottleneck_channel_defs::out, top_out>>;

    template <typename TIME>
    using sim_top =
        cadmium::modeling::devs::coupling<TIME, empty_iports, top_oports, top_submodels, empty_eic,
                                          top_eoc, top_ic, cadmium::engine::devs::first_imminent>;

} // namespace

TEST_CASE("coordinator: same-instant frames on one port both traverse the channel") {
    cadmium::engine::devs::coordinator<sim_top, double> coord;
    coord.init(0.0);

    int frames_seen         = 0;
    std::uint16_t first_seq = 0;
    auto next               = coord.next();
    while (next < 5.0) {
        coord.collect_outputs(next);
        const auto &out = coord.template outbox_port<top_out>();
        if (out.has_value()) {
            ++frames_seen;
            if (frames_seen == 1) {
                first_seq = out->pkt.seq_nr;
            }
        }
        coord.advance_simulation(next);
        next = coord.next();
    }

    // Both same-instant emissions must arrive; FIFO preserves the SELECT
    // serialization order (shot_a listed first -> seq 1 first).
    CHECK(frames_seen == 2);
    CHECK(first_seq == 1);
}
