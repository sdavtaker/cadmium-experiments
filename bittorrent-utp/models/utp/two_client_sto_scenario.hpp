// SPDX-License-Identifier: BSD-2-Clause
/**
 * Stochastic-pass sibling of test_pair_validation.cpp's wired_pair harness:
 * the same two-socket topology (source -> A -> lossy_channel -> B, plus the
 * return channel B -> A for ACKs), but with lossy_channel in place of
 * bottleneck_channel on both directions, and a single shared URNG threaded
 * through the channels' transitions (the only stochastic atoms here; the
 * sockets and source stay deterministic). Per stage-2's det-before-sto
 * split, only the channel type changes — everything else mirrors the
 * deterministic scenario A/B topology and parameters.
 *
 * p (drop probability) and the RNG seed are the scenario's actual
 * variables (per this project's own stage-2 sto-pass design, not any
 * external spec — BEP 29 governs the transport protocol, not network loss
 * statistics): jitter_min/jitter_max default to a modest window relative to
 * the propagation delay, wide enough to exercise reordering without
 * dominating RTT dynamics.
 *
 * Used by both the Catch2 tests (test_two_client_sto.cpp) and the
 * standalone bt-utp-s2-sto executable (main_s2_sto.cpp) so the scenario
 * logic isn't duplicated between them.
 */
#pragma once

#include <cadmium/logger/cadmium_log.hpp>
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../../msg/app_chunk.hpp"
#include "bottleneck_channel.hpp"
#include "lossy_channel.hpp"
#include "sim_time.hpp"
#include "traffic_source.hpp"
#include "utp_socket.hpp"
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <vector>

namespace bt_utp {

    template <SimTime TIME, typename URNG = std::mt19937> struct wired_pair_sto {
        using sdefs     = utp_socket_defs_t<app_chunk>;
        using frame_t   = typename sdefs::frame_t;
        using channel_t = lossy_channel<TIME, URNG, frame_t>;
        using chan_defs = bottleneck_channel_defs_t<frame_t>;
        using sock_t    = utp_socket<TIME, app_chunk>;
        using source_t  = traffic_source<TIME>;

        source_t source;
        sock_t a, b;
        channel_t ab, ba;

        TIME now{};
        TIME last_source{}, last_a{}, last_b{}, last_ab{}, last_ba{};

        std::vector<typename sdefs::deliver_ind> delivered_b{};

        wired_pair_sto(peer_id self_a, utp_constants ka, peer_id self_b, utp_constants kb,
                       app_chunk chunk, TIME prop_delay, double rate_ab, double rate_ba,
                       double drop_prob_ab, double jitter_min_ab, double jitter_max_ab)
            : source(self_b, chunk), a(self_a, ka), b(self_b, kb),
              ab(prop_delay, rate_ab, drop_prob_ab, jitter_min_ab, jitter_max_ab),
              ba(prop_delay, rate_ba, 0.0, 0.0, 0.0) {}

        static TIME abs_next(TIME last, TIME sigma) {
            return sigma == std::numeric_limits<TIME>::infinity()
                       ? std::numeric_limits<TIME>::infinity()
                       : last + sigma;
        }

        typename cadmium::make_message_box<typename sock_t::input_ports>::type
        sock_frame_box(const frame_t &f) {
            typename cadmium::make_message_box<typename sock_t::input_ports>::type box;
            cadmium::get_message<typename sdefs::net_in>(box).emplace(f);
            return box;
        }
        typename cadmium::make_message_box<typename sock_t::input_ports>::type
        sock_combined_box(const std::optional<frame_t> &net_in_frame,
                          const std::optional<typename sdefs::send_req> &send_req_msg) {
            typename cadmium::make_message_box<typename sock_t::input_ports>::type box;
            if (net_in_frame.has_value()) {
                cadmium::get_message<typename sdefs::net_in>(box).emplace(*net_in_frame);
            }
            if (send_req_msg.has_value()) {
                cadmium::get_message<typename sdefs::app_send>(box).emplace(*send_req_msg);
            }
            return box;
        }
        typename cadmium::make_message_box<typename channel_t::input_ports>::type
        chan_box(const frame_t &f) {
            typename cadmium::make_message_box<typename channel_t::input_ports>::type box;
            cadmium::get_message<typename chan_defs::in>(box).emplace(f);
            return box;
        }

        /// Advances until either every atom is passive or the next event
        /// time would exceed t_max (can return an earlier time than t_max).
        /// A single URNG drives both channels' stochastic transitions;
        /// source/sockets stay deterministic.
        TIME run(TIME t_max, URNG &rng) {
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
                    src_out = cadmium::get_message<typename source_t::defs::out>(source.output());
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
                    ab.internal_transition(rng);
                    last_ab = now;
                }
                if (ba_up) {
                    ba.internal_transition(rng);
                    last_ba = now;
                }

                if (src_out.has_value() || ba_out.has_value()) {
                    a.external_transition(now - last_a, sock_combined_box(ba_out, src_out));
                    last_a = now;
                }
                if (a_out_net.has_value()) {
                    ab.external_transition(now - last_ab, chan_box(*a_out_net), rng);
                    last_ab = now;
                }
                if (ab_out.has_value()) {
                    b.external_transition(now - last_b, sock_frame_box(*ab_out));
                    last_b = now;
                }
                if (b_out_net.has_value()) {
                    ba.external_transition(now - last_ba, chan_box(*b_out_net), rng);
                    last_ba = now;
                }
                if (b_out_deliver.has_value()) {
                    delivered_b.push_back(*b_out_deliver);
                }
                (void)a_out_deliver;
            }
        }
    };

    struct sto_scenario_result {
        double finish               = 0.0;
        bool all_delivered          = false;
        std::uint64_t dropped_ab    = 0;
        std::uint64_t retransmits_a = 0;
        double cwnd_final_a         = 0.0;
    };

    /// Runs one instance of the stochastic two-client scenario (same
    /// topology/rate/prop_delay as the deterministic scenario A, with
    /// lossy_channel replacing bottleneck_channel on the A->B direction):
    /// a bulk transfer of total_bytes over a rate-capped, lossy+jittery
    /// link, seeded for reproducibility.
    template <typename URNG = std::mt19937>
    sto_scenario_result
    run_two_client_sto(std::uint32_t seed, double drop_prob, std::uint64_t total_bytes = 2'000'000,
                       double t_max = 120.0, double jitter_min = 0.0, double jitter_max = 0.02) {
        constexpr double rate_ab    = 1'000'000.0;
        constexpr double rate_ba    = 100'000'000.0;
        constexpr double prop_delay = 0.05;

        utp_constants k{};
        wired_pair_sto<double, URNG> h{
            1,          k,       2,       k,         app_chunk{1, total_bytes},
            prop_delay, rate_ab, rate_ba, drop_prob, jitter_min,
            jitter_max};
        URNG rng(seed);
        sto_scenario_result r;
        r.finish = h.run(t_max, rng);
        r.all_delivered =
            h.delivered_b.size() == 1 && h.delivered_b[0].payload == app_chunk{1, total_bytes};
        r.dropped_ab     = h.ab.state.dropped_loss;
        r.retransmits_a  = h.a.state.retransmits;
        const auto *conn = h.a.connection(2);
        r.cwnd_final_a   = conn != nullptr ? conn->cwnd : 0.0;
        return r;
    }

} // namespace bt_utp
