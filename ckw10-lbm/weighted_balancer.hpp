// SPDX-License-Identifier: BSD-2-Clause
/**
 * CKW10 §7.2.2 Weighted Balancer (WB)
 *
 * STDEVS atomic model.  Routes incoming tasks to out₁ (S1) or out₂ (S2)
 * using a uniform random draw against the balancing factor b_f:
 *   r < b_f  →  out₁  (S1 gets the task with probability b_f)
 *   r ≥ b_f  →  out₂  (S2 gets the task with probability 1 - b_f)
 *
 * STDEVS specification (DEVS-RND form, CKW10 §7.2.2):
 *   X = T × {inp₁}
 *   Y = T × {out₁, out₂}
 *   S = T × {out₁, out₂} × ℝ⁺  (last task, chosen port, σ)
 *   δ_ext((w,p,σ), e, (xᵥ,xₚ), r) = (xᵥ, p̃, 0)
 *       p̃ = out₁ if r < b_f, else out₂
 *   δ_int((w,p,σ), r) = (w, p, ∞)
 *   λ(w, p, σ)        = (w, p)
 *   ta(w, p, σ)       = σ   (0 when ready to route, ∞ when idle)
 */

#ifndef CADMIUM_EXPERIMENTS_WB_HPP
#define CADMIUM_EXPERIMENTS_WB_HPP

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "load_generator.hpp"
#include <limits>
#include <random>

namespace ckw10_lbm {

    struct wb_defs {
        struct inp : public cadmium::in_port<int> {};
        struct out1 : public cadmium::out_port<int> {};
        struct out2 : public cadmium::out_port<int> {};
    };

    template <typename TIME, typename URNG = std::mt19937> class weighted_balancer {
      public:
        const double b_f; // balancing factor ∈ [0,1]

        explicit weighted_balancer(double bf) : b_f(bf) {}

        struct state_type {
            int task     = 0;
            bool to_s1   = true;
            double sigma = std::numeric_limits<double>::infinity();
        };
        state_type state{};

        using input_ports  = std::tuple<wb_defs::inp>;
        using output_ports = std::tuple<wb_defs::out1, wb_defs::out2>;

        void internal_transition(URNG &) {
            state.sigma = std::numeric_limits<double>::infinity();
        }

        void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type box,
                                 URNG &rng) {
            const auto &msg = cadmium::get_message<wb_defs::inp>(box);
            if (msg.has_value()) {
                std::uniform_real_distribution<double> u(0.0, 1.0);
                state.task  = msg.value();
                state.to_s1 = (u(rng) < b_f);
                state.sigma = 0.0; // fire immediately
            }
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            if (state.to_s1)
                cadmium::get_message<wb_defs::out1>(box) = state.task;
            else
                cadmium::get_message<wb_defs::out2>(box) = state.task;
            return box;
        }

        TIME time_advance() const {
            return static_cast<TIME>(state.sigma);
        }
    };

} // namespace ckw10_lbm

#endif // CADMIUM_EXPERIMENTS_WB_HPP
