// SPDX-License-Identifier: BSD-2-Clause
/**
 * CKW10 §7.2.1 Load Generator (LG)
 *
 * STDEVS atomic model for the load generator in the Load Balancer Model.
 * Generates tasks with exponentially distributed inter-departure times.
 *
 * STDEVS specification (Activity c / DEVS-RND form, CKW10 eq. just before §7.2.2):
 *   X = ∅
 *   Y = {(task₁, out₁)}
 *   S = ℝ⁺  (current inter-departure time σ)
 *   δ_int(s, r) = -(1/d_r) · log(r),   r ~ U(0,1)
 *   δ_ext = ∅  (no inputs)
 *   λ(s)  = (task₁, out₁)
 *   ta(s) = s
 */

#ifndef CADMIUM_EXPERIMENTS_LG_HPP
#define CADMIUM_EXPERIMENTS_LG_HPP

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include <random>
#include <stdexcept>

namespace ckw10_lbm {

    struct lg_defs {
        struct out : public cadmium::out_port<int> {};
    };

    template <typename TIME, typename URNG = std::mt19937> class load_generator {
      public:
        const double d_r; // mean departure rate (tasks per time unit)

        explicit load_generator(double rate) : d_r(rate) {
            state.sigma = 1.0 / rate; // first event at mean inter-departure time
        }

        struct state_type {
            double sigma = 1.0; // current inter-departure time
        };
        state_type state{};

        using input_ports  = std::tuple<>;
        using output_ports = std::tuple<lg_defs::out>;

        void internal_transition(URNG &rng) {
            std::exponential_distribution<double> dist(d_r);
            state.sigma = dist(rng);
        }

        void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type,
                                 URNG &) {
            throw std::logic_error("LG has no inputs");
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            cadmium::get_message<lg_defs::out>(box) = 1;
            return box;
        }

        TIME time_advance() const {
            return static_cast<TIME>(state.sigma);
        }
    };

} // namespace ckw10_lbm

#endif // CADMIUM_EXPERIMENTS_LG_HPP
