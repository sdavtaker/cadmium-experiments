// SPDX-License-Identifier: BSD-2-Clause
/**
 * Deterministic bottleneck channel: single-server FIFO with a rate cap,
 * fixed propagation delay, optional finite queue (tail drop) and optional
 * deterministic drop pattern (drop-every-Nth, the deterministic-pass
 * stand-in for random loss).
 *
 * Queuing delay emerges from the rate cap: departure(frame) =
 * max(now, server_free) + wire_size/rate, delivery = departure + prop_delay.
 * This is what lets LEDBAT's delay backoff show up deterministically.
 *
 * Classic DEVS atomic (no randomness); the STDEVS lossy_channel variant
 * replaces the drop pattern and adds delivery jitter with the same ports.
 *
 * Generic over the carried frame type FRAME (anything exposing
 * wire_size()): defaults to net_frame, so every existing net_frame-based
 * channel keeps its original port types unchanged. utp_socket's own
 * net_in/net_out ports carry utp_frame<PAYLOAD> instead (it additionally
 * tags which application payloads complete at each packet), so pairing a
 * channel with a socket instantiates bottleneck_channel<TIME,
 * utp_frame<PAYLOAD>>.
 */
#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../../msg/net_frame.hpp"
#include "sim_time.hpp"
#include <cstdint>
#include <deque>
#include <limits>
#include <ostream>
#include <stdexcept>

namespace bt_utp {

    template <typename FRAME> struct bottleneck_channel_defs_t {
        struct in : public cadmium::in_port<FRAME> {};
        struct out : public cadmium::out_port<FRAME> {};
    };

    /// Original name/type, preserved as an alias so every existing
    /// net_frame-based channel (declared as bottleneck_channel<TIME>, i.e.
    /// FRAME defaulted to net_frame) keeps identical port types.
    using bottleneck_channel_defs = bottleneck_channel_defs_t<net_frame>;

    // SimTime (sim_time.hpp): double by default, but not restricted to
    // std::floating_point — an exact-arithmetic TIME type (e.g.
    // cdcommons::time::decimal) must be usable too, since DEVS causality is
    // only exact under exact arithmetic (source-VDW14-devs-time-datatype.md).
    template <SimTime TIME, typename FRAME = net_frame> class bottleneck_channel {
        using defs = bottleneck_channel_defs_t<FRAME>;

      public:
        /// prop_delay: one-way propagation delay added after service.
        /// rate: service rate in bytes/second — a throughput, not a TIME
        /// duration, so it stays double regardless of TIME (matching
        /// utp_constants' own config values); the byte/rate division is
        /// computed in double and converted once via seconds_converter, so
        /// TIME types without operator/ (e.g. cdcommons decimal) work too.
        /// queue_capacity_bytes: tail-drop threshold; 0 = unbounded.
        /// drop_every_nth: deterministically drop every Nth arriving frame
        /// (the Nth, 2Nth, ...); 0 = disabled.
        bottleneck_channel(TIME prop_delay, double rate, std::uint64_t queue_capacity_bytes = 0,
                           std::uint64_t drop_every_nth = 0)
            : prop_delay_(prop_delay), rate_(rate), queue_capacity_bytes_(queue_capacity_bytes),
              drop_every_nth_(drop_every_nth) {
            if (!(rate_ > 0.0)) {
                throw std::invalid_argument("bottleneck_channel: rate must be > 0");
            }
            if (prop_delay_ < TIME{}) {
                throw std::invalid_argument("bottleneck_channel: prop_delay must be >= 0");
            }
        }

        struct in_transit {
            TIME delivery_rem; // time until the frame exits the far end
            TIME service_rem;  // time until the frame clears the server
            FRAME frame;
        };

        struct state_type {
            std::deque<in_transit> pending{};
            TIME server_free_rem{};
            std::uint64_t arrivals         = 0;
            std::uint64_t received         = 0;
            std::uint64_t forwarded        = 0;
            std::uint64_t dropped_nth      = 0;
            std::uint64_t dropped_overflow = 0;

            [[nodiscard]] std::uint64_t queued_bytes() const {
                std::uint64_t b = 0;
                for (const auto &e : pending) {
                    if (e.service_rem > TIME{}) {
                        b += e.frame.wire_size();
                    }
                }
                return b;
            }

            friend std::ostream &operator<<(std::ostream &os, const state_type &s) {
                return os << "q:" << s.pending.size() << " queued_bytes:" << s.queued_bytes()
                          << " rx:" << s.received << " tx:" << s.forwarded
                          << " dropN:" << s.dropped_nth << " dropQ:" << s.dropped_overflow;
            }
        };
        state_type state{};

        using input_ports  = std::tuple<typename defs::in>;
        using output_ports = std::tuple<typename defs::out>;

        void internal_transition() {
            const TIME sigma = time_advance();
            age(sigma);
            if (!state.pending.empty() && !(state.pending.front().delivery_rem > TIME{})) {
                ++state.forwarded;
                state.pending.pop_front();
            }
        }

        void external_transition(TIME elapsed,
                                 typename cadmium::make_message_box<input_ports>::type box) {
            age(elapsed);
            const auto &slot = cadmium::get_message<typename defs::in>(box);
            if (!slot.has_value()) {
                return;
            }
            const FRAME &frame = *slot;
            ++state.received;
            ++state.arrivals;
            if (drop_every_nth_ != 0 && state.arrivals % drop_every_nth_ == 0) {
                ++state.dropped_nth;
                return;
            }
            const std::uint64_t size = frame.wire_size();
            if (queue_capacity_bytes_ != 0 && state.queued_bytes() + size > queue_capacity_bytes_) {
                ++state.dropped_overflow;
                return;
            }
            const double service_seconds = static_cast<double>(size) / rate_;
            const TIME service_rem =
                state.server_free_rem + seconds_converter<TIME>::convert(service_seconds);
            state.server_free_rem = service_rem;
            state.pending.push_back({service_rem + prop_delay_, service_rem, frame});
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            if (!state.pending.empty()) {
                cadmium::get_message<typename defs::out>(box).emplace(state.pending.front().frame);
            }
            return box;
        }

        TIME time_advance() const {
            if (state.pending.empty()) {
                return std::numeric_limits<TIME>::infinity();
            }
            return state.pending.front().delivery_rem;
        }

      private:
        void age(TIME dt) {
            if (!(dt > TIME{})) {
                return;
            }
            for (auto &e : state.pending) {
                e.delivery_rem = e.delivery_rem > dt ? e.delivery_rem - dt : TIME{};
                e.service_rem  = e.service_rem > dt ? e.service_rem - dt : TIME{};
            }
            state.server_free_rem =
                state.server_free_rem > dt ? state.server_free_rem - dt : TIME{};
        }

        TIME prop_delay_;
        double rate_;
        std::uint64_t queue_capacity_bytes_;
        std::uint64_t drop_every_nth_;
    };

} // namespace bt_utp
