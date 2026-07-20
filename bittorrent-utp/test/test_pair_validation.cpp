// SPDX-License-Identifier: BSD-2-Clause
/**
 * Deterministic uTP pair validation: two utp_socket instances connected
 * through real bottleneck_channel instances (not the zero-latency direct
 * wiring used in test_utp_socket.cpp), driving:
 *
 *   scenario A — LEDBAT convergence: a bulk transfer over a rate-capped
 *   link should hold its queuing delay near CCONTROL_TARGET rather than
 *   filling the channel's queue, and steady-state throughput should track
 *   the channel rate. This is LEDBAT's defining behavior — backing off on
 *   delay before loss, unlike TCP.
 *
 *   scenario B — deterministic drop recovery: the same transfer over a
 *   channel with a fixed drop-every-Nth pattern must still complete, with
 *   every drop repaired via fast-retransmit or RTO.
 *
 * Both scenarios are fully deterministic (fixed channel params, a fixed
 * drop pattern, no randomness anywhere) so their outcomes are exact,
 * reproducible golden values, not statistical ranges.
 */
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../models/utp/bottleneck_channel.hpp"
#include "../models/utp/traffic_source.hpp"
#include "../models/utp/utp_socket.hpp"
#include "../msg/app_chunk.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

    using bt_utp::app_chunk;
    using bt_utp::bottleneck_channel;
    using bt_utp::bottleneck_channel_defs_t;
    using bt_utp::peer_id;
    using bt_utp::traffic_source;
    using bt_utp::utp_constants;

    using sock_t    = bt_utp::utp_socket<double, app_chunk>;
    using sdefs     = bt_utp::utp_socket_defs_t<app_chunk>;
    using frame_t   = sdefs::frame_t; // utp_frame<app_chunk>, not net_frame
    using chan_defs = bottleneck_channel_defs_t<frame_t>;
    using channel_t = bottleneck_channel<double, frame_t>;
    using source_t  = traffic_source<double>;

    using sock_in_box_t = cadmium::make_message_box<sock_t::input_ports>::type;
    using chan_in_box_t = cadmium::make_message_box<channel_t::input_ports>::type;

    sock_in_box_t sock_frame_box(const frame_t &f) {
        sock_in_box_t box;
        cadmium::get_message<sdefs::net_in>(box).emplace(f);
        return box;
    }
    sock_in_box_t sock_send_box(const sdefs::send_req &req) {
        sock_in_box_t box;
        cadmium::get_message<sdefs::app_send>(box).emplace(req);
        return box;
    }
    chan_in_box_t chan_box(const frame_t &f) {
        chan_in_box_t box;
        cadmium::get_message<chan_defs::in>(box).emplace(f);
        return box;
    }

    /// Two uTP sockets connected through real (nonzero-latency,
    /// rate-capped) channels in both directions, plus a one-shot traffic
    /// source feeding socket A. A minimal hand-rolled classic-DEVS
    /// scheduler: each atom's time_advance() is the remaining time from
    /// its own last update, so the harness tracks a shared "now" and, per
    /// step, catches every imminent atom up via internal_transition() and
    /// every receiving atom up via external_transition(elapsed, box) with
    /// elapsed = now - <atom's last update time>.
    struct wired_pair {
        source_t source;
        sock_t a, b;
        channel_t ab, ba; // A->B (bulk data), B->A (acks)

        double now         = 0.0;
        double last_source = 0.0, last_a = 0.0, last_b = 0.0, last_ab = 0.0, last_ba = 0.0;

        std::vector<sdefs::deliver_ind> delivered_b{};

        wired_pair(peer_id self_a, utp_constants ka, peer_id self_b, utp_constants kb,
                   app_chunk chunk, double prop_delay, double rate_ab, double rate_ba,
                   std::uint64_t queue_cap_ab = 0, std::uint64_t drop_every_nth_ab = 0)
            : source(self_b, chunk), a(self_a, ka), b(self_b, kb),
              ab(prop_delay, rate_ab, queue_cap_ab, drop_every_nth_ab), ba(prop_delay, rate_ba) {}

        static double abs_next(double last, double sigma) {
            return sigma == std::numeric_limits<double>::infinity()
                       ? std::numeric_limits<double>::infinity()
                       : last + sigma;
        }

        /// Simultaneity tolerance for "is this atom imminent at `now`":
        /// absolute event times reach `now` via different chains of
        /// floating-point additions per atom, so exact `==` can split
        /// events that should fire together into separate micro-steps on
        /// ULP-level rounding differences. 1e-9 s is many orders of
        /// magnitude below any real time granularity these scenarios use
        /// (microsecond-scale timestamps at the finest) and comfortably
        /// above double's relative epsilon at these magnitudes.
        static constexpr double simultaneity_eps = 1e-9;

        static bool imminent_at(double an_x, double now) {
            return std::abs(an_x - now) <= simultaneity_eps;
        }

        /// Runs until quiescent (all atoms passive) or t_max, whichever
        /// comes first. Returns the final simulation time reached.
        double run(double t_max) {
            for (;;) {
                const double an_source = abs_next(last_source, source.time_advance());
                const double an_a      = abs_next(last_a, a.time_advance());
                const double an_b      = abs_next(last_b, b.time_advance());
                const double an_ab     = abs_next(last_ab, ab.time_advance());
                const double an_ba     = abs_next(last_ba, ba.time_advance());

                double next = an_source;
                next        = std::min(next, an_a);
                next        = std::min(next, an_b);
                next        = std::min(next, an_ab);
                next        = std::min(next, an_ba);

                if (next == std::numeric_limits<double>::infinity() || next > t_max) {
                    return now;
                }
                now = next;

                // Capture outputs of every imminent atom before mutating
                // any state, then apply internal transitions, then route.
                const bool src_up = imminent_at(an_source, now);
                const bool a_up   = imminent_at(an_a, now);
                const bool b_up   = imminent_at(an_b, now);
                const bool ab_up  = imminent_at(an_ab, now);
                const bool ba_up  = imminent_at(an_ba, now);

                std::optional<sdefs::send_req> src_out;
                std::optional<frame_t> a_out_net, ab_out, ba_out;
                std::optional<sdefs::deliver_ind> a_out_deliver, b_out_deliver;
                std::optional<frame_t> b_out_net;

                if (src_up) {
                    src_out = cadmium::get_message<source_t::defs::out>(source.output());
                }
                if (a_up) {
                    const auto out = a.output();
                    a_out_net      = cadmium::get_message<sdefs::net_out>(out);
                    a_out_deliver  = cadmium::get_message<sdefs::app_deliver>(out);
                }
                if (b_up) {
                    const auto out = b.output();
                    b_out_net      = cadmium::get_message<sdefs::net_out>(out);
                    b_out_deliver  = cadmium::get_message<sdefs::app_deliver>(out);
                }
                if (ab_up) {
                    ab_out = cadmium::get_message<chan_defs::out>(ab.output());
                }
                if (ba_up) {
                    ba_out = cadmium::get_message<chan_defs::out>(ba.output());
                }

                if (src_up) {
                    source.internal_transition();
                    last_source = now;
                }
                if (a_up) {
                    a.internal_transition();
                    last_a = now;
                }
                if (b_up) {
                    b.internal_transition();
                    last_b = now;
                }
                if (ab_up) {
                    ab.internal_transition();
                    last_ab = now;
                }
                if (ba_up) {
                    ba.internal_transition();
                    last_ba = now;
                }

                // Route: source -> a (app_send), ab.out -> b (net_in),
                // ba.out -> a (net_in), a.net_out -> ab (in),
                // b.net_out -> ba (in). A component that is BOTH imminent
                // and a receiver this same instant (e.g. a fires
                // internally and also receives a routed frame) gets a
                // second external_transition call at elapsed=0, matching
                // classic-DEVS external-input semantics after an internal
                // event at the same instant.
                if (src_out.has_value()) {
                    a.external_transition(now - last_a, sock_send_box(*src_out));
                    last_a = now;
                }
                if (a_out_net.has_value()) {
                    ab.external_transition(now - last_ab, chan_box(*a_out_net));
                    last_ab = now;
                }
                if (ab_out.has_value()) {
                    b.external_transition(now - last_b, sock_frame_box(*ab_out));
                    last_b = now;
                }
                if (b_out_net.has_value()) {
                    ba.external_transition(now - last_ba, chan_box(*b_out_net));
                    last_ba = now;
                }
                if (ba_out.has_value()) {
                    a.external_transition(now - last_a, sock_frame_box(*ba_out));
                    last_a = now;
                }
                if (b_out_deliver.has_value()) {
                    delivered_b.push_back(*b_out_deliver);
                }
                (void)a_out_deliver; // A never receives application data in these scenarios
            }
        }
    };

} // namespace

TEST_CASE("deterministic pair: LEDBAT holds queuing delay near target under a bulk transfer") {
    // 1 MB/s cap, 50 ms one-way propagation, ample queue (no tail-drop —
    // isolates LEDBAT's own delay-based backoff), no loss. Ack path is
    // effectively uncapped so the forward (data) path is the only
    // bottleneck under test.
    constexpr double rate_ab            = 1'000'000.0; // bytes/s
    constexpr double rate_ba            = 100'000'000.0;
    constexpr double prop_delay         = 0.05;      // s
    constexpr std::uint64_t total_bytes = 2'000'000; // 2 MB

    utp_constants k{};
    wired_pair h{1,
                 k,
                 2,
                 k,
                 app_chunk{1, total_bytes},
                 prop_delay,
                 rate_ab,
                 rate_ba,
                 /*queue_cap_ab=*/0,
                 /*drop_every_nth_ab=*/0};

    const double finish = h.run(60.0);

    REQUIRE(h.delivered_b.size() == 1);
    CHECK(h.delivered_b[0].payload == app_chunk{1, total_bytes});
    CHECK(h.a.state.retransmits == 0); // no loss configured: nothing to repair

    // Analytic lower bound: serialization time at the rate cap plus one
    // round trip for the handshake, with zero queuing delay.
    const double analytic_min = static_cast<double>(total_bytes) / rate_ab + 2 * prop_delay;
    CHECK(finish > analytic_min);

    // Golden value: this scenario is fully deterministic (fixed channel
    // params, no randomness anywhere), so its completion time is an exact,
    // reproducible number — verified by running the scenario (measured
    // 3.096 s; 2.1 s analytic minimum). The tolerance is a few percent of
    // the measured value, not a loosely guessed bound, to absorb only
    // floating-point accumulation differences across environments.
    CHECK_THAT(finish, Catch::Matchers::WithinAbs(3.096, 0.05));

    // LEDBAT's defining property: it holds a *bounded* standing queue near
    // the target delay rather than filling the channel's buffer — total
    // elapsed time stays within a small multiple of the serialization time,
    // not the unbounded blowup an uncontrolled sender would show.
    CHECK(finish < analytic_min + 4.0 * k.target_delay + 2.0);

    // Congestion window ramped up substantially from the 150-byte floor
    // (measured final cwnd ~347 KB) — successful slow-start growth, not a
    // window stuck near its minimum.
    REQUIRE(h.a.connection(2) != nullptr);
    CHECK(h.a.connection(2)->cwnd > 50'000.0);

    // Channel queue never grew large: at 1 MB/s a 100ms-target standing
    // queue is ~100 KB: comfortably bounded relative to the 2 MB transfer.
    CHECK(h.ab.state.dropped_overflow == 0);
}

TEST_CASE("deterministic pair: transfer completes and is repaired under scripted packet loss") {
    constexpr double rate_ab               = 1'000'000.0;
    constexpr double rate_ba               = 100'000'000.0;
    constexpr double prop_delay            = 0.05;
    constexpr std::uint64_t total_bytes    = 500'000;
    constexpr std::uint64_t drop_every_nth = 20; // deterministic stand-in for random loss

    utp_constants k{};
    wired_pair h{1,
                 k,
                 2,
                 k,
                 app_chunk{7, total_bytes},
                 prop_delay,
                 rate_ab,
                 rate_ba,
                 /*queue_cap_ab=*/0,
                 drop_every_nth};

    const double finish = h.run(60.0);

    REQUIRE(h.delivered_b.size() == 1);
    CHECK(h.delivered_b[0].payload == app_chunk{7, total_bytes});
    CHECK(h.ab.state.dropped_nth > 0);                      // the scripted pattern actually fired
    CHECK(h.a.state.retransmits >= h.ab.state.dropped_nth); // every drop got repaired

    // Retransmissions cross the same lossy channel, so some of them get
    // dropped and repaired again too (measured: 59 original drops, 280
    // total retransmits — under 5x compounding). A generous upper multiple
    // guards against a future regression causing a retransmit storm without
    // over-fitting to the exact compounding ratio.
    CHECK(h.a.state.retransmits < 20 * h.ab.state.dropped_nth);

    // Regression guard, not a tight pin: loss recovery has more emergent
    // timing variability than the lossless scenario. Measured 6.32 s
    // against a ~0.55 s analytic minimum (serialization + one RTT); this
    // bound is well above the measured value with headroom for legitimate
    // future changes to retry timing, while still catching pathological
    // slowdowns.
    CHECK(finish < 15.0);
}
