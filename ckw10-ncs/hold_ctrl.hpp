// SPDX-License-Identifier: BSD-2-Clause
/**
 * CKW10 §7.3 NCS — Zero-Order Hold + Unity-Feedback Controller
 *
 * Deterministic DEVS atomic.  Receives delayed noisy samples y'(kh+τ)
 * from the NIRD, holds them, and outputs u = Ref − y_held (Ref=1).
 * Fires immediately (σ=0) on each new sample; fires once at t=0 to
 * announce the initial control signal u=1 (plant starts at y=0).
 *
 * State: (y_held, σ)
 */

#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include <limits>

namespace ckw10_ncs {

    struct hold_ctrl_defs {
        struct in_y : public cadmium::in_port<double> {};
        struct out_u : public cadmium::out_port<double> {};
    };

    template <typename TIME> class hold_ctrl {
      public:
        static constexpr double REF = 1.0;

        struct state_type {
            double y_held = 0.0;
            double sigma  = 0.0; // fire at t=0 → sends initial u=REF
        };
        state_type state{};

        using input_ports  = std::tuple<hold_ctrl_defs::in_y>;
        using output_ports = std::tuple<hold_ctrl_defs::out_u>;

        void internal_transition() {
            state.sigma = std::numeric_limits<double>::infinity();
        }

        void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type box) {
            const auto &msg = cadmium::get_message<hold_ctrl_defs::in_y>(box);
            if (msg.has_value())
                state.y_held = msg.value();
            state.sigma = 0.0;
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            cadmium::get_message<hold_ctrl_defs::out_u>(box) = REF - state.y_held;
            return box;
        }

        TIME time_advance() const {
            return static_cast<TIME>(state.sigma);
        }
    };

} // namespace ckw10_ncs
