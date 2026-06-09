// SPDX-License-Identifier: BSD-2-Clause
/**
 * CKW10 §7.2.3 Server (S1, S2)
 *
 * STDEVS atomic model.  Single-server, no-queue (M/M/1/1 behaviour):
 * tasks arriving while the server is busy are silently dropped.
 * Service time is exponentially distributed with mean s_t.
 *
 * STDEVS specification (DEVS-RND form, CKW10 §7.2.3):
 *   X = T × {inp₁}
 *   Y = T × {out₁}
 *   S = T × {false, true} × ℝ⁺   (last task w, busy flag, σ)
 *   δ_ext((w,busy,σ), e, (xᵥ,xₚ), r):
 *       if ¬busy : (xᵥ, true,  -(1/μ) log r)   μ = 1/s_t
 *       if busy  : (w,  true,  σ − e)           (drop arriving task)
 *   δ_int((w,busy,σ), r) = (w, false, ∞)
 *   λ(w, busy, σ)        = (w, out₁)
 *   ta(w, busy, σ)       = σ
 */

#ifndef CADMIUM_EXPERIMENTS_SERVER_HPP
#define CADMIUM_EXPERIMENTS_SERVER_HPP

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include <limits>
#include <random>

namespace ckw10_lbm {

    struct server_defs {
        struct inp : public cadmium::in_port<int> {};
        struct out : public cadmium::out_port<int> {};
    };

    template <typename TIME, typename URNG = std::mt19937> class server {
      public:
        const double mu; // service rate = 1 / mean_service_time

        explicit server(double mean_service_time) : mu(1.0 / mean_service_time) {}

        struct state_type {
            int task     = 0;
            bool busy    = false;
            double sigma = std::numeric_limits<double>::infinity();
        };
        state_type state{};

        using input_ports  = std::tuple<server_defs::inp>;
        using output_ports = std::tuple<server_defs::out>;

        void internal_transition(URNG &) {
            state.busy  = false;
            state.sigma = std::numeric_limits<double>::infinity();
        }

        void external_transition(TIME elapsed,
                                 typename cadmium::make_message_box<input_ports>::type box,
                                 URNG &rng) {
            const auto &msg = cadmium::get_message<server_defs::inp>(box);
            if (!msg.has_value())
                return;
            if (!state.busy) {
                std::exponential_distribution<double> dist(mu);
                state.task  = msg.value();
                state.busy  = true;
                state.sigma = dist(rng);
            } else {
                state.sigma -= static_cast<double>(elapsed); // drop task, update remaining time
            }
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            cadmium::get_message<server_defs::out>(box) = state.task;
            return box;
        }

        TIME time_advance() const {
            return static_cast<TIME>(state.sigma);
        }
    };

} // namespace ckw10_lbm

#endif // CADMIUM_EXPERIMENTS_SERVER_HPP
