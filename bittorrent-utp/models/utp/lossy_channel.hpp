// SPDX-License-Identifier: BSD-2-Clause
/**
 * STDEVS lossy channel: same single-server FIFO rate cap and propagation
 * delay as bottleneck_channel, but each arrival is independently dropped
 * with probability drop_prob (Bernoulli), and a surviving frame's departure
 * gets an added jitter sample ~ U(jitter_min, jitter_max) drawn once at
 * arrival time (the only point external_transition has access to the
 * shared URNG).
 *
 * The FIFO server occupancy itself stays deterministic: jitter never
 * changes when the *server* becomes free for the next arrival, only when
 * an already-served frame is actually emitted at the far end. That is what
 * lets overlapping jitter windows reorder frames relative to their arrival
 * order (the "natural reordering" this model exists to produce) without
 * disturbing the queueing/throughput math bottleneck_channel already
 * validates deterministically. Because of that, time_advance()/output()
 * pick the pending frame with the *smallest* remaining delivery time, not
 * the front of arrival order (bottleneck_channel's shortcut, valid only
 * because it never reorders).
 */
#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../../msg/net_frame.hpp"
#include "bottleneck_channel.hpp"
#include "sim_time.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <ostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace bt_utp {

    // SimTime (sim_time.hpp): see bottleneck_channel.hpp for why this isn't
    // restricted to std::floating_point.
    template <SimTime TIME, typename URNG = std::mt19937, typename FRAME = net_frame>
    class lossy_channel {
        using defs = bottleneck_channel_defs_t<FRAME>;

      public:
        /// prop_delay/rate: same meaning as bottleneck_channel.
        /// drop_prob: independent per-frame drop probability, in [0, 1).
        /// jitter_min_seconds/jitter_max_seconds: additional delay sampled
        /// ~ U(min, max), added on top of the deterministic FIFO-service +
        /// prop_delay departure time; 0 <= min <= max.
        lossy_channel(TIME prop_delay, double rate, double drop_prob, double jitter_min_seconds,
                      double jitter_max_seconds)
            : prop_delay_(prop_delay), rate_(rate), drop_prob_(drop_prob),
              jitter_dist_(jitter_min_seconds, jitter_max_seconds) {
            if (!(rate_ > 0.0)) {
                throw std::invalid_argument("lossy_channel: rate must be > 0");
            }
            if (prop_delay_ < TIME{}) {
                throw std::invalid_argument("lossy_channel: prop_delay must be >= 0");
            }
            if (!(drop_prob_ >= 0.0 && drop_prob_ < 1.0)) {
                throw std::invalid_argument("lossy_channel: drop_prob must be in [0, 1)");
            }
            if (!(jitter_min_seconds >= 0.0 && jitter_max_seconds >= jitter_min_seconds)) {
                throw std::invalid_argument("lossy_channel: jitter_max must be >= jitter_min >= 0");
            }
        }

        struct in_transit {
            TIME delivery_rem; // time until this frame exits the far end
            FRAME frame;
        };

        struct state_type {
            std::vector<in_transit> pending{};
            TIME server_free_rem{};
            std::uint64_t arrivals     = 0;
            std::uint64_t received     = 0;
            std::uint64_t forwarded    = 0;
            std::uint64_t dropped_loss = 0;

            friend std::ostream &operator<<(std::ostream &os, const state_type &s) {
                return os << "q:" << s.pending.size() << " rx:" << s.received
                          << " tx:" << s.forwarded << " dropL:" << s.dropped_loss;
            }
        };
        state_type state{};

        using input_ports  = std::tuple<typename defs::in>;
        using output_ports = std::tuple<typename defs::out>;

        void internal_transition(URNG &) {
            const TIME sigma = time_advance();
            age(sigma);
            const auto it = imminent_it();
            if (it != state.pending.end()) {
                ++state.forwarded;
                state.pending.erase(it);
            }
        }

        void external_transition(TIME elapsed,
                                 typename cadmium::make_message_box<input_ports>::type box,
                                 URNG &rng) {
            age(elapsed);
            const auto &slot = cadmium::get_message<typename defs::in>(box);
            if (!slot.has_value()) {
                return;
            }
            const FRAME &frame = *slot;
            ++state.received;
            ++state.arrivals;

            // Only touch the shared URNG when the outcome is actually
            // random: a channel configured with drop_prob=0 and/or a
            // point-interval jitter window (min==max) is deterministic in
            // effect, and must not perturb the RNG stream any co-scheduled
            // stochastic channel relies on for reproducibility (e.g. the
            // ba leg in wired_pair_sto, configured this way specifically
            // to be deterministic).
            if (drop_prob_ > 0.0) {
                std::bernoulli_distribution drop(drop_prob_);
                if (drop(rng)) {
                    ++state.dropped_loss;
                    return;
                }
            }

            const std::uint64_t size     = frame.wire_size();
            const double service_seconds = static_cast<double>(size) / rate_;
            const TIME service_rem =
                state.server_free_rem + seconds_converter<TIME>::convert(service_seconds);
            state.server_free_rem = service_rem;

            const double jitter_seconds =
                jitter_dist_.a() < jitter_dist_.b() ? jitter_dist_(rng) : jitter_dist_.a();
            const TIME delivery_rem =
                service_rem + prop_delay_ + seconds_converter<TIME>::convert(jitter_seconds);
            state.pending.push_back({delivery_rem, frame});
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            const auto it = imminent_it();
            if (it != state.pending.end()) {
                cadmium::get_message<typename defs::out>(box).emplace(it->frame);
            }
            return box;
        }

        TIME time_advance() const {
            const auto it = imminent_it();
            return it == state.pending.end() ? std::numeric_limits<TIME>::infinity()
                                             : it->delivery_rem;
        }

      private:
        [[nodiscard]] typename std::vector<in_transit>::const_iterator imminent_it() const {
            return std::min_element(state.pending.begin(), state.pending.end(),
                                    [](const in_transit &a, const in_transit &b) {
                                        return a.delivery_rem < b.delivery_rem;
                                    });
        }
        [[nodiscard]] typename std::vector<in_transit>::iterator imminent_it() {
            return std::min_element(state.pending.begin(), state.pending.end(),
                                    [](const in_transit &a, const in_transit &b) {
                                        return a.delivery_rem < b.delivery_rem;
                                    });
        }

        void age(TIME dt) {
            if (!(dt > TIME{})) {
                return;
            }
            for (auto &e : state.pending) {
                e.delivery_rem = e.delivery_rem > dt ? e.delivery_rem - dt : TIME{};
            }
            state.server_free_rem =
                state.server_free_rem > dt ? state.server_free_rem - dt : TIME{};
        }

        TIME prop_delay_;
        double rate_;
        double drop_prob_;
        std::uniform_real_distribution<double> jitter_dist_;
    };

} // namespace bt_utp
