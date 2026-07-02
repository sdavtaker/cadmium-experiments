// SPDX-License-Identifier: BSD-2-Clause
/**
 * CKW10 §7.3 NCS — Network Induced Random Delay (NIRD)
 *
 * STDEVS atomic.  Receives a noisy sample y', delays it by
 * τ ~ U(0, τ_max), then outputs y'.  Single-buffer: arrivals while busy
 * are dropped (consistent with the paper's single-channel network model).
 *
 * STDEVS specification (DEVS-RND form):
 *   X = ℝ × {in_y}
 *   Y = ℝ × {out_y}
 *   S = (y' ∈ ℝ, σ ∈ ℝ⁺)
 *
 *   δ_ext((y',σ), e, x, r):
 *       if passive (σ=∞): y' ← x; σ ← r·τ_max
 *       if busy:          σ ← σ − e  (drop new arrival)
 *   δ_int((y',σ), r):  σ ← ∞
 *   λ(y',σ):           y'
 *   ta(y',σ):          σ
 */

#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include <limits>
#include <random>

namespace ckw10_ncs {

    struct nird_defs {
        struct in_y : public cadmium::in_port<double> {};
        struct out_y : public cadmium::out_port<double> {};
    };

    template <typename TIME, typename URNG = std::mt19937> class nird {
      public:
        const double tau_max;

        explicit nird(double tau_max_) : tau_max(tau_max_) {}

        struct state_type {
            double y_delayed = 0.0;
            double sigma     = std::numeric_limits<double>::infinity();
        };
        state_type state{};

        using input_ports  = std::tuple<nird_defs::in_y>;
        using output_ports = std::tuple<nird_defs::out_y>;

        void internal_transition(URNG &) {
            state.sigma = std::numeric_limits<double>::infinity();
        }

        void external_transition(TIME elapsed,
                                 typename cadmium::make_message_box<input_ports>::type box,
                                 URNG &rng) {
            bool passive = state.sigma == std::numeric_limits<double>::infinity();
            if (passive) {
                const auto &msg = cadmium::get_message<nird_defs::in_y>(box);
                if (msg.has_value()) {
                    state.y_delayed = msg.value();
                    std::uniform_real_distribution<double> dist(0.0, tau_max);
                    state.sigma = dist(rng);
                }
            } else {
                state.sigma -= static_cast<double>(elapsed);
            }
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            cadmium::get_message<nird_defs::out_y>(box) = state.y_delayed;
            return box;
        }

        TIME time_advance() const {
            return static_cast<TIME>(state.sigma);
        }
    };

} // namespace ckw10_ncs
