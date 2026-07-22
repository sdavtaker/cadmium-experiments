// SPDX-License-Identifier: BSD-2-Clause
/**
 * Unit tests for the STDEVS lossy_channel (stochastic-pass sibling of
 * bottleneck_channel): Bernoulli(p) loss, jitter-induced reordering, and
 * seed reproducibility, driven directly (no coordinator) with a
 * hand-threaded std::mt19937, mirroring test_msg_channel.cpp's direct-call
 * style for bottleneck_channel.
 */
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../models/utp/lossy_channel.hpp"
#include "../msg/net_frame.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {

    using bt_utp::bottleneck_channel_defs;
    using bt_utp::lossy_channel;
    using bt_utp::net_frame;
    using bt_utp::packet_type;

    using RNG       = std::mt19937;
    using channel_t = lossy_channel<double, RNG>;

    net_frame make_frame(std::uint16_t seq, std::uint64_t payload) {
        net_frame f{};
        f.src              = 1;
        f.dst              = 2;
        f.pkt.type         = packet_type::st_data;
        f.pkt.seq_nr       = seq;
        f.pkt.payload_size = payload;
        return f;
    }

    typename cadmium::make_message_box<channel_t::input_ports>::type box_with(const net_frame &f) {
        typename cadmium::make_message_box<channel_t::input_ports>::type box;
        cadmium::get_message<bottleneck_channel_defs::in>(box).emplace(f);
        return box;
    }

} // namespace

TEST_CASE("lossy channel: zero drop and zero jitter matches bottleneck_channel's analytic FIFO "
          "timing") {
    // Same scenario as the bottleneck_channel analytic test: R = 1000 B/s,
    // D = 0.05 s, two 1000 B frames back-to-back must exit at 1.05 and
    // 2.05. drop_prob=0 and a degenerate jitter U(0,0) make the RNG draws
    // irrelevant, so this is exact regardless of seed.
    channel_t ch{0.05, 1000.0, 0.0, 0.0, 0.0};
    RNG rng(1);

    ch.external_transition(0.0, box_with(make_frame(1, 1000 - bt_utp::utp_header_bytes)), rng);
    REQUIRE_THAT(ch.time_advance(), Catch::Matchers::WithinAbs(1.05, 1e-9));

    ch.external_transition(0.0, box_with(make_frame(2, 1000 - bt_utp::utp_header_bytes)), rng);
    REQUIRE_THAT(ch.time_advance(), Catch::Matchers::WithinAbs(1.05, 1e-9));

    auto out1 = ch.output();
    REQUIRE(cadmium::get_message<bottleneck_channel_defs::out>(out1).has_value());
    CHECK(cadmium::get_message<bottleneck_channel_defs::out>(out1)->pkt.seq_nr == 1);
    ch.internal_transition(rng);

    REQUIRE_THAT(ch.time_advance(), Catch::Matchers::WithinAbs(1.0, 1e-9));
    auto out2 = ch.output();
    REQUIRE(cadmium::get_message<bottleneck_channel_defs::out>(out2).has_value());
    CHECK(cadmium::get_message<bottleneck_channel_defs::out>(out2)->pkt.seq_nr == 2);
    ch.internal_transition(rng);

    CHECK(ch.time_advance() == std::numeric_limits<double>::infinity());
    CHECK(ch.state.forwarded == 2);
    CHECK(ch.state.dropped_loss == 0);
}

TEST_CASE("lossy channel: empirical drop rate matches the configured probability") {
    constexpr double p = 0.3;
    constexpr int n    = 4000;
    channel_t ch{0.0, 1e9, p, 0.0, 0.0}; // fast server: nothing ever queues
    RNG rng(42);

    for (int i = 0; i < n; ++i) {
        ch.external_transition(0.0, box_with(make_frame(static_cast<std::uint16_t>(i), 100)), rng);
    }

    CHECK(ch.state.received == static_cast<std::uint64_t>(n));
    const double observed_p = static_cast<double>(ch.state.dropped_loss) / n;
    // Wide statistical tolerance (a fixed seed already makes this exact and
    // reproducible; the tolerance just avoids over-fitting to one draw).
    CHECK_THAT(observed_p, Catch::Matchers::WithinAbs(p, 0.03));
}

TEST_CASE("lossy channel: overlapping jitter can reorder deliveries relative to arrival order") {
    // Two frames served back-to-back by a fast link (negligible serialization
    // gap) with a wide jitter window: independent per-arrival jitter draws
    // make it possible for the second-arrived frame to depart before the
    // first. Not every seed reorders (that's the point of jitter being
    // random) — search a small, fixed range of seeds and require at least
    // one to reorder, which exercises the actual mechanism rather than
    // pinning a single magic seed to one PRNG implementation's sequence.
    bool saw_reorder = false;
    for (std::uint32_t seed = 1; seed <= 50 && !saw_reorder; ++seed) {
        channel_t ch{0.0, 1e9, 0.0, 0.0, 1.0}; // jitter ~ U(0, 1.0) seconds
        RNG rng(seed);
        ch.external_transition(0.0, box_with(make_frame(1, 100)), rng);
        ch.external_transition(0.0, box_with(make_frame(2, 100)), rng);

        auto out        = ch.output();
        const auto &msg = cadmium::get_message<bottleneck_channel_defs::out>(out);
        REQUIRE(msg.has_value());
        if (msg->pkt.seq_nr == 2) {
            saw_reorder = true;
        }
    }
    CHECK(saw_reorder);
}

TEST_CASE("lossy channel: same seed reproduces identical state after identical arrivals") {
    // Reproducibility is a property of (seed, sequence of elapsed+arrivals)
    // determinism, independent of scheduling/delivery-draining logic (that's
    // exercised separately above) — so this drives external_transition
    // directly with a fixed elapsed sequence and compares the resulting
    // state, without needing a scheduler loop at all.
    auto run = [](std::uint32_t seed) {
        channel_t ch{0.02, 5000.0, 0.2, 0.0, 0.3};
        RNG rng(seed);
        for (std::uint16_t i = 1; i <= 20; ++i) {
            ch.external_transition(i == 1 ? 0.0 : 0.01, box_with(make_frame(i, 200)), rng);
        }
        std::vector<double> pending_delivery_rem;
        for (const auto &e : ch.state.pending) {
            pending_delivery_rem.push_back(e.delivery_rem);
        }
        return std::make_tuple(pending_delivery_rem, ch.state.received, ch.state.dropped_loss);
    };

    const auto [pending1, rx1, drop1] = run(7);
    const auto [pending2, rx2, drop2] = run(7);
    CHECK(pending1 == pending2);
    CHECK(rx1 == rx2);
    CHECK(drop1 == drop2);
}

TEST_CASE("lossy channel: invalid construction parameters are rejected") {
    CHECK_THROWS_AS(channel_t(0.05, 0.0, 0.1, 0.0, 0.1), std::invalid_argument);
    CHECK_THROWS_AS(channel_t(-0.01, 1000.0, 0.1, 0.0, 0.1), std::invalid_argument);
    CHECK_THROWS_AS(channel_t(0.05, 1000.0, 1.0, 0.0, 0.1), std::invalid_argument);
    CHECK_THROWS_AS(channel_t(0.05, 1000.0, -0.1, 0.0, 0.1), std::invalid_argument);
    CHECK_THROWS_AS(channel_t(0.05, 1000.0, 0.1, 0.5, 0.1), std::invalid_argument);
}

TEST_CASE("lossy channel: output()/internal_transition() reject being called while passive") {
    channel_t ch{0.05, 1000.0, 0.1, 0.0, 0.1}; // no arrivals yet: passive
    RNG rng(1);
    REQUIRE(ch.time_advance() == std::numeric_limits<double>::infinity());
    CHECK_THROWS_AS(ch.output(), std::logic_error);
    CHECK_THROWS_AS(ch.internal_transition(rng), std::logic_error);
}
