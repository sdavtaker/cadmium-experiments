// SPDX-License-Identifier: BSD-2-Clause
/**
 * CKW10 §7.3 NCS — Periodic Sensor with AWGN (combined Sense+Sample + AWGN generator)
 *
 * STDEVS atomic.  Fires every H seconds and outputs y'(kH) = y_held + η,
 * where η ~ N(0, v_η) is drawn via Box–Müller at each firing.
 * External inputs from the QSS integrator update y_held (ZOH).
 *
 * STDEVS specification (DEVS-RND form):
 *   X = ℝ × {in_y}             — QSS state crossing from int1
 *   Y = ℝ × {out_y}            — noisy sample y'(kH)
 *   S = (y_held ∈ ℝ, η ∈ ℝ, σ ∈ ℝ⁺)
 *
 *   δ_ext((y,η,σ), e, x): y_held ← x; σ ← σ − e
 *   δ_int((y,η,σ), r):    η ← Box–Müller(r); σ ← H
 *   λ(y,η,σ):             y + η
 *   ta(y,η,σ):            σ
 */

#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <random>

namespace ckw10_ncs {

    struct sensor_defs {
        struct in_y : public cadmium::in_port<double> {};
        struct out_y : public cadmium::out_port<double> {};
    };

    template <typename TIME, typename URNG = std::mt19937> class sensor {
      public:
        const double H;     // sampling period (seconds)
        const double v_eta; // noise variance

        sensor(double period, double noise_variance)
            : H(period), v_eta(noise_variance), state{0.0, 0.0, period} {}

        struct state_type {
            double y_held;
            double eta;
            double sigma; // time to next sample; initialized to H in constructor
        };
        state_type state;

        using input_ports  = std::tuple<sensor_defs::in_y>;
        using output_ports = std::tuple<sensor_defs::out_y>;

        void internal_transition(URNG &rng) {
            // Box–Müller: draw two uniforms, produce N(0, v_η)
            std::uniform_real_distribution<double> u01(std::numeric_limits<double>::min(), 1.0);
            double r1 = u01(rng);
            double r2 = u01(rng);
            state.eta = std::sqrt(-2.0 * std::log(r1)) * std::cos(2.0 * std::numbers::pi * r2) *
                        std::sqrt(v_eta);
            state.sigma = H;
        }

        void external_transition(TIME elapsed,
                                 typename cadmium::make_message_box<input_ports>::type box,
                                 URNG &) {
            const auto &msg = cadmium::get_message<sensor_defs::in_y>(box);
            if (msg.has_value())
                state.y_held = msg.value();
            state.sigma -= static_cast<double>(elapsed);
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            cadmium::get_message<sensor_defs::out_y>(box) = state.y_held + state.eta;
            return box;
        }

        TIME time_advance() const {
            return static_cast<TIME>(state.sigma);
        }
    };

} // namespace ckw10_ncs
