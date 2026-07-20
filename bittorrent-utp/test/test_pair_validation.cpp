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
 *
 * The scheduler's simultaneity check uses EXACT equality, not an epsilon
 * tolerance: epsilon-fudged simultaneity in DEVS hides float/double's
 * accumulated rounding error instead of measuring it, and can silently
 * merge or split events that exact DEVS semantics say should or shouldn't
 * coincide — an unbounded, undetected causality error, not a cosmetic
 * flakiness issue.
 *
 * The harness is templated on TIME (SimTime, sim_time.hpp) so scenario A
 * also runs under cdcommons::time::decimal<-6, int64_t> — this project's
 * own established exact-arithmetic time type for real experiments — as a
 * causality reference for double, and the two are asserted to agree. That
 * agreement is itself the useful result: it demonstrates, rather than
 * assumes, that double is trustworthy at this scale (VDW14's own finding
 * is that float/double causality errors are "invisible in a short run,
 * only reliably observed over tens of thousands of seconds," so this is a
 * real, if narrow, check). RationalTime (../../rational_time.hpp) was
 * tried first and rejected: its naive long long numerator/denominator
 * overflows within ~30-40 chained heterogeneous-denominator operations —
 * confirmed with UBSan on a probe mirroring this model's own RTT EWMA
 * update — which a real transfer's RTT/RTO update count exceeds by a wide
 * margin. decimal's fixed scale (raw*10^Exp, exact integer +/-, no
 * denominator of any kind) sidesteps that failure mode entirely.
 */
#include <cadmium/logger/cadmium_log.hpp>
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../models/utp/bottleneck_channel.hpp"
#include "../models/utp/sim_time.hpp"
#include "../models/utp/traffic_source.hpp"
#include "../models/utp/utp_socket.hpp"
#include "../msg/app_chunk.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cdcommons/time/decimal.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

    using bt_utp::app_chunk;
    using bt_utp::bottleneck_channel;
    using bt_utp::bottleneck_channel_defs_t;
    using bt_utp::peer_id;
    using bt_utp::seconds_converter;
    using bt_utp::SimTime;
    using bt_utp::traffic_source;
    using bt_utp::utp_constants;

    /// Exact-arithmetic reference: microsecond resolution matches uTP's own
    /// wire-header timestamp granularity, and int64_t gives ~292,000 years
    /// of headroom at that resolution — no risk of magnitude overflow for
    /// any scenario here. Unlike RationalTime, there is no denominator to
    /// grow: every operation is a plain fixed-scale integer add/subtract.
    using exact_time = cdcommons::time::decimal<-6, std::int64_t>;

    using sdefs     = bt_utp::utp_socket_defs_t<app_chunk>;
    using frame_t   = sdefs::frame_t; // utp_frame<app_chunk>, not net_frame
    using chan_defs = bottleneck_channel_defs_t<frame_t>;

    template <SimTime TIME> using sock_t    = bt_utp::utp_socket<TIME, app_chunk>;
    template <SimTime TIME> using channel_t = bottleneck_channel<TIME, frame_t>;
    template <SimTime TIME> using source_t  = traffic_source<TIME>;

    template <SimTime TIME>
    typename cadmium::make_message_box<typename sock_t<TIME>::input_ports>::type
    sock_frame_box(const frame_t &f) {
        typename cadmium::make_message_box<typename sock_t<TIME>::input_ports>::type box;
        cadmium::get_message<typename sdefs::net_in>(box).emplace(f);
        return box;
    }
    /// Combines an optional net_in frame and an optional app_send request
    /// into a single input bag, so a socket that receives both at the same
    /// simulation instant gets one external_transition call — matching
    /// utp_socket's own net_in-before-app_send ordering internally, instead
    /// of the harness driving two separate calls in whichever order it
    /// happens to route them.
    template <SimTime TIME>
    typename cadmium::make_message_box<typename sock_t<TIME>::input_ports>::type
    sock_combined_box(const std::optional<frame_t> &net_in_frame,
                      const std::optional<typename sdefs::send_req> &send_req_msg) {
        typename cadmium::make_message_box<typename sock_t<TIME>::input_ports>::type box;
        if (net_in_frame.has_value()) {
            cadmium::get_message<typename sdefs::net_in>(box).emplace(*net_in_frame);
        }
        if (send_req_msg.has_value()) {
            cadmium::get_message<typename sdefs::app_send>(box).emplace(*send_req_msg);
        }
        return box;
    }
    template <SimTime TIME>
    typename cadmium::make_message_box<typename channel_t<TIME>::input_ports>::type
    chan_box(const frame_t &f) {
        typename cadmium::make_message_box<typename channel_t<TIME>::input_ports>::type box;
        cadmium::get_message<typename chan_defs::in>(box).emplace(f);
        return box;
    }

    /// Two uTP sockets connected through real (nonzero-latency,
    /// rate-capped) channels in both directions, plus a one-shot traffic
    /// source feeding socket A. A minimal hand-rolled classic-DEVS
    /// scheduler: each atom's time_advance() is the remaining time from
    /// its own last update, so the harness tracks a shared "now" and, per
    /// step, catches every imminent atom up via internal_transition() and
    /// every receiving atom up via external_transition(elapsed, box) with
    /// elapsed = now - <atom's last update time>. Simultaneity is exact
    /// equality (see file header) — TIME must therefore be exact-arithmetic
    /// to be trustworthy, which is exactly what this harness cross-checks.
    template <SimTime TIME> struct wired_pair {
        source_t<TIME> source;
        sock_t<TIME> a, b;
        channel_t<TIME> ab, ba; // A->B (bulk data), B->A (acks)

        TIME now{};
        TIME last_source{}, last_a{}, last_b{}, last_ab{}, last_ba{};

        std::vector<typename sdefs::deliver_ind> delivered_b{};

        wired_pair(peer_id self_a, utp_constants ka, peer_id self_b, utp_constants kb,
                   app_chunk chunk, TIME prop_delay, double rate_ab, double rate_ba,
                   std::uint64_t queue_cap_ab = 0, std::uint64_t drop_every_nth_ab = 0)
            : source(self_b, chunk), a(self_a, ka), b(self_b, kb),
              ab(prop_delay, rate_ab, queue_cap_ab, drop_every_nth_ab), ba(prop_delay, rate_ba) {}

        static TIME abs_next(TIME last, TIME sigma) {
            return sigma == std::numeric_limits<TIME>::infinity()
                       ? std::numeric_limits<TIME>::infinity()
                       : last + sigma;
        }

        /// Advances until either every atom is passive (next event time is
        /// infinity) or the next event time would exceed t_max, whichever
        /// comes first — in the t_max case, the last event actually
        /// processed can be earlier than t_max, not t_max itself. Returns
        /// that final simulation time reached.
        TIME run(TIME t_max) {
            for (;;) {
                const TIME an_source = abs_next(last_source, source.time_advance());
                const TIME an_a      = abs_next(last_a, a.time_advance());
                const TIME an_b      = abs_next(last_b, b.time_advance());
                const TIME an_ab     = abs_next(last_ab, ab.time_advance());
                const TIME an_ba     = abs_next(last_ba, ba.time_advance());

                TIME next = an_source;
                next      = std::min(next, an_a);
                next      = std::min(next, an_b);
                next      = std::min(next, an_ab);
                next      = std::min(next, an_ba);

                if (next == std::numeric_limits<TIME>::infinity() || next > t_max) {
                    return now;
                }
                now = next;

                // Capture outputs of every imminent atom before mutating
                // any state, then apply internal transitions, then route.
                // Exact equality, not an epsilon tolerance: see file header.
                const bool src_up = an_source == now;
                const bool a_up   = an_a == now;
                const bool b_up   = an_b == now;
                const bool ab_up  = an_ab == now;
                const bool ba_up  = an_ba == now;

                std::optional<typename sdefs::send_req> src_out;
                std::optional<frame_t> a_out_net, ab_out, ba_out;
                std::optional<typename sdefs::deliver_ind> a_out_deliver, b_out_deliver;
                std::optional<frame_t> b_out_net;

                if (src_up) {
                    src_out =
                        cadmium::get_message<typename source_t<TIME>::defs::out>(source.output());
                }
                if (a_up) {
                    const auto out = a.output();
                    a_out_net      = cadmium::get_message<typename sdefs::net_out>(out);
                    a_out_deliver  = cadmium::get_message<typename sdefs::app_deliver>(out);
                }
                if (b_up) {
                    const auto out = b.output();
                    b_out_net      = cadmium::get_message<typename sdefs::net_out>(out);
                    b_out_deliver  = cadmium::get_message<typename sdefs::app_deliver>(out);
                }
                if (ab_up) {
                    ab_out = cadmium::get_message<typename chan_defs::out>(ab.output());
                }
                if (ba_up) {
                    ba_out = cadmium::get_message<typename chan_defs::out>(ba.output());
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
                // b.net_out -> ba (in). A can receive both src_out
                // (app_send) and ba_out (net_in) at the same instant, so
                // those two are combined into a single input bag and
                // delivered via one external_transition call — matching
                // classic-DEVS "simultaneous inputs on multiple ports are
                // one event" semantics and utp_socket's own internal
                // net_in-before-app_send ordering (see utp_socket.hpp),
                // rather than driving two separate calls in routing order.
                if (src_out.has_value() || ba_out.has_value()) {
                    a.external_transition(now - last_a, sock_combined_box<TIME>(ba_out, src_out));
                    last_a = now;
                }
                if (a_out_net.has_value()) {
                    ab.external_transition(now - last_ab, chan_box<TIME>(*a_out_net));
                    last_ab = now;
                }
                if (ab_out.has_value()) {
                    b.external_transition(now - last_b, sock_frame_box<TIME>(*ab_out));
                    last_b = now;
                }
                if (b_out_net.has_value()) {
                    ba.external_transition(now - last_ba, chan_box<TIME>(*b_out_net));
                    last_ba = now;
                }
                if (b_out_deliver.has_value()) {
                    delivered_b.push_back(*b_out_deliver);
                }
                (void)a_out_deliver; // A never receives application data in these scenarios
            }
        }
    };

    // Scenario A parameters, shared across TIME instantiations: rate is
    // always double (a throughput, not a TIME quantity — see
    // bottleneck_channel.hpp), and prop_delay/t_max are converted to TIME
    // via seconds_converter, the same customization point utp_socket
    // itself uses for its own config constants.
    constexpr double rate_ab_sc = 1'000'000.0, rate_ba_sc = 100'000'000.0;
    constexpr double prop_delay_sc           = 0.05;
    constexpr std::uint64_t scenario_a_bytes = 2'000'000; // 2 MB
    constexpr double t_max_sc                = 60.0;

    template <SimTime TIME> TIME scenario_a_finish() {
        utp_constants k{};
        wired_pair<TIME> h{1,
                           k,
                           2,
                           k,
                           app_chunk{1, scenario_a_bytes},
                           seconds_converter<TIME>::convert(prop_delay_sc),
                           rate_ab_sc,
                           rate_ba_sc,
                           /*queue_cap_ab=*/500'000, // finite: see dropped_overflow check below
                           /*drop_every_nth_ab=*/0};
        return h.run(seconds_converter<TIME>::convert(t_max_sc));
    }

} // namespace

TEST_CASE("deterministic pair: LEDBAT holds queuing delay near target under a bulk transfer") {
    utp_constants k{};
    wired_pair<double> h{1,
                         k,
                         2,
                         k,
                         app_chunk{1, scenario_a_bytes},
                         prop_delay_sc,
                         rate_ab_sc,
                         rate_ba_sc,
                         /*queue_cap_ab=*/500'000, // finite: see dropped_overflow check below
                         /*drop_every_nth_ab=*/0};

    const double finish = h.run(t_max_sc);

    REQUIRE(h.delivered_b.size() == 1);
    CHECK(h.delivered_b[0].payload == app_chunk{1, scenario_a_bytes});
    CHECK(h.a.state.retransmits == 0); // no loss configured: nothing to repair

    // Analytic lower bound: serialization time at the rate cap plus one
    // round trip for the handshake, with zero queuing delay.
    const double analytic_min =
        static_cast<double>(scenario_a_bytes) / rate_ab_sc + 2 * prop_delay_sc;
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
    // queue is ~100 KB, so a 500 KB cap is comfortable headroom for LEDBAT's
    // normal operation but still finite — a regression that let the queue
    // run away toward the full 2 MB transfer would trip this, unlike an
    // unbounded (0) cap where the check could never fail.
    CHECK(h.ab.state.dropped_overflow == 0);
}

TEST_CASE("deterministic pair: transfer completes and is repaired under scripted packet loss") {
    constexpr double rate_ab               = 1'000'000.0;
    constexpr double rate_ba               = 100'000'000.0;
    constexpr double prop_delay            = 0.05;
    constexpr std::uint64_t total_bytes    = 500'000;
    constexpr std::uint64_t drop_every_nth = 20; // deterministic stand-in for random loss

    utp_constants k{};
    wired_pair<double> h{1,
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

TEST_CASE("exact-arithmetic reference: decimal<-6> agrees with double on scenario A") {
    // The point of this test: don't assume double's scheduling arithmetic
    // is trustworthy, measure it — run the identical scenario (same
    // parameters, same topology, same code path via the TIME template
    // parameter) under an exact fixed-point time type and check it agrees
    // with the double run's own result. See the file header for why this
    // is the correct way to build confidence in float/double time, instead
    // of an epsilon-tolerant simultaneity check that would hide any
    // divergence rather than reveal it.
    const double finish_double          = scenario_a_finish<double>();
    const exact_time finish_exact       = scenario_a_finish<exact_time>();
    const double finish_exact_as_double = cadmium::log::to_sim_double(finish_exact);

    INFO("double finish=" << finish_double << " exact finish=" << finish_exact << " ("
                          << finish_exact_as_double << ")");

    // Tight tolerance: microsecond-resolution decimal's only imprecision is
    // bounded, single-step rounding at each byte/rate conversion and RTT
    // EWMA divide (never compounding, unlike a growing rational
    // denominator or float's magnitude-dependent error) — any REAL
    // divergence (the VDW14 causality-violation phenomenon: a gained or
    // lost retransmission or RTT somewhere in the run) would show up as a
    // difference far larger than microsecond-scale rounding.
    CHECK_THAT(finish_exact_as_double, Catch::Matchers::WithinAbs(finish_double, 1e-3));
}
