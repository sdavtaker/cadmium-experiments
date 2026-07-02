// SPDX-License-Identifier: BSD-2-Clause
/**
 * CKW10 §7.3 Networked Control System (NCS) — Validation Experiment
 *
 * Replicates the NCS case study from Castro, Kofman, Wainer (SIMULATION 2010):
 *
 *   Plant:     G_P(s) = 1/(s² + 0.8s)   [state-space: ẋ₁=x₂, ẋ₂=u−0.8x₂]
 *   Control:   unity-feedback ZOH, u = 1 − y_held
 *   Sensor:    ZOH samples at h=1s; adds AWGN η ~ N(0, v_η=0.001)
 *   Network:   delay τ ~ U(0, τ_max), swept τ_max ∈ {0.1, 0.2, …, 0.8}
 *   Cost:      MSE = (1/T) ∫₀ᵀ (1−y(t))² dt
 *   Run:       T=10000s, N=15 replications per τ_max  (--quick: T=200s, N=3)
 *
 * Topology:
 *   hold_ctrl  → plant_wsum.in<0>   (u = 1 − y_held)
 *   int2.out_q → plant_wsum.in<1>   (x₂ for −0.8·x₂ term)
 *   plant_wsum → int2.in_u          (ẋ₂)
 *   int2.out_q → int1.in_u          (ẋ₁ = x₂)
 *   int1.out_q → sensor.in_y        (QSS boundary crossing → ZOH update)
 *   sensor     → nird.in_y          (noisy sample)
 *   nird       → hold_ctrl.in_y     (delayed sample → controller update)
 *
 * QSS integration uses cadmium's qss1_integrator and qss_wsum<2>.
 * STDEVS models (sensor, nird) take URNG& at stochastic transitions.
 * hold_ctrl is deterministic.
 *
 * Manual event loop — mixes DEVS (QSS) and STDEVS (sensor, nird) models
 * without a cadmium coupled-model wrapper.
 *
 * ELAPSED-TIME CONVENTION: last_* is updated after EVERY transition (internal
 * or external) so that the next external_transition receives elapsed = t_now −
 * t_last_event.  Missing this update causes double-advance Zeno cascades.
 */

#include <cadmium/basic_model/qss/qss1_integrator.hpp>
#include <cadmium/basic_model/qss/qss_wsum.hpp>

#include "hold_ctrl.hpp"
#include "nird.hpp"
#include "sensor.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <string_view>

using namespace cadmium::basic_models::qss;
using namespace ckw10_ncs;

using TIME = double;
using RNG  = std::mt19937;

// ── Experiment parameters ──────────────────────────────────────────────────

static constexpr double H     = 1.0;   // sampling period (s)
static constexpr double V_ETA = 0.001; // AWGN variance
static constexpr double DQ    = 0.01;  // QSS1 absolute quantum (both integrators)
static constexpr int N_REPS   = 15;    // replications for full run

// ── Port aliases ───────────────────────────────────────────────────────────

using int_out = qss1_integrator_defs::out_q;
using int_in  = qss1_integrator_defs::in_u;
using ws2_out = qss_wsum_defs<2>::out;
using ws2_in0 = qss_wsum_defs<2>::in<0>;
using ws2_in1 = qss_wsum_defs<2>::in<1>;
using hc_out  = hold_ctrl_defs::out_u;
using hc_in   = hold_ctrl_defs::in_y;
using sen_out = sensor_defs::out_y;
using sen_in  = sensor_defs::in_y;
using nd_out  = nird_defs::out_y;
using nd_in   = nird_defs::in_y;

static constexpr double INF = std::numeric_limits<double>::infinity();

// ── Message-box helpers ────────────────────────────────────────────────────

template <typename PORT, typename BOX> static void set_msg(BOX &box, double val) {
    cadmium::get_message<PORT>(box) = val;
}

template <typename M> static auto make_box() {
    return typename cadmium::make_message_box<typename M::input_ports>::type{};
}

// ── One simulation run ─────────────────────────────────────────────────────

static double run_once(double tau_max, double t_end, RNG::result_type seed) {
    // Models
    qss1_integrator<TIME> int1(0.0, DQ, 0.0); // x₁ = y,  ẋ₁ = x₂
    qss1_integrator<TIME> int2(0.0, DQ, 0.0); // x₂ = ẏ,  ẋ₂ = u − 0.8x₂
    qss_wsum<2, TIME> wsum({1.0, -0.8});      // ẋ₂ = 1·u + (−0.8)·x₂
    sensor<TIME, RNG> sen(H, V_ETA);
    nird<TIME, RNG> nd(tau_max);
    hold_ctrl<TIME> hc;

    RNG rng(seed);

    // Scheduled next-event times (infinity = passive / nothing pending)
    double t_int1 = int1.time_advance(); // 0
    double t_int2 = int2.time_advance(); // 0
    double t_wsum = INF;
    double t_sen  = sen.time_advance(); // H
    double t_nd   = INF;
    double t_hc   = hc.time_advance(); // 0 → fires at t=0 with u=1

    // Timestamps for elapsed-time computation.
    // Updated after EVERY transition (internal or external) to avoid
    // double-advancing the QSS integrator state.
    double last_int1 = 0, last_int2 = 0, last_wsum = 0;
    double last_sen = 0, last_nd = 0, last_hc = 0;

    // MSE accumulation: y(t) is piecewise-constant in QSS1.
    double mse_sum  = 0.0;
    double last_y   = 0.0;
    double t_last_y = 0.0;

    while (true) {
        double t_next = std::min({t_int1, t_int2, t_wsum, t_sen, t_nd, t_hc});
        if (t_next >= t_end)
            break;

        // ── Collect outputs ────────────────────────────────────────────────
        std::optional<double> out_int1, out_int2, out_wsum, out_sen, out_nd, out_hc;

        if (t_int1 == t_next)
            out_int1 = cadmium::get_message<int_out>(int1.output()).value();
        if (t_int2 == t_next)
            out_int2 = cadmium::get_message<int_out>(int2.output()).value();
        if (t_wsum == t_next)
            out_wsum = cadmium::get_message<ws2_out>(wsum.output()).value();
        if (t_sen == t_next)
            out_sen = cadmium::get_message<sen_out>(sen.output()).value();
        if (t_nd == t_next)
            out_nd = cadmium::get_message<nd_out>(nd.output()).value();
        if (t_hc == t_next)
            out_hc = cadmium::get_message<hc_out>(hc.output()).value();

        // ── MSE accumulation before advancing int1 ─────────────────────────
        if (out_int1) {
            mse_sum += (1.0 - last_y) * (1.0 - last_y) * (t_next - t_last_y);
            last_y   = *out_int1;
            t_last_y = t_next;
        }

        // ── Internal transitions (update last_* so elapsed is correct later)
        if (t_int1 == t_next) {
            int1.internal_transition();
            last_int1 = t_next;
            t_int1    = t_next + int1.time_advance();
        }
        if (t_int2 == t_next) {
            int2.internal_transition();
            last_int2 = t_next;
            t_int2    = t_next + int2.time_advance();
        }
        if (t_wsum == t_next) {
            wsum.internal_transition();
            last_wsum = t_next;
            t_wsum    = INF;
        }
        if (t_sen == t_next) {
            sen.internal_transition(rng);
            last_sen = t_next;
            t_sen    = t_next + sen.time_advance();
        }
        if (t_nd == t_next) {
            nd.internal_transition(rng);
            last_nd = t_next;
            t_nd    = INF;
        }
        if (t_hc == t_next) {
            hc.internal_transition();
            last_hc = t_next;
            t_hc    = INF;
        }

        // ── Route outputs → external transitions ───────────────────────────

        // int1.out_q → sensor.in_y (ZOH update of y)
        if (out_int1) {
            auto box = make_box<sensor<TIME, RNG>>();
            set_msg<sen_in>(box, *out_int1);
            sen.external_transition(t_next - last_sen, box, rng);
            last_sen = t_next;
            t_sen    = t_next + sen.time_advance();
        }

        // int2.out_q → int1.in_u  (ẋ₁ = x₂)
        if (out_int2) {
            auto box = make_box<qss1_integrator<TIME>>();
            set_msg<int_in>(box, *out_int2);
            int1.external_transition(t_next - last_int1, box);
            last_int1 = t_next;
            t_int1    = t_next + int1.time_advance();
        }

        // int2.out_q → wsum.in<1>  (x₂ for −0.8·x₂)
        if (out_int2) {
            auto box = make_box<qss_wsum<2, TIME>>();
            set_msg<ws2_in1>(box, *out_int2);
            wsum.external_transition(t_next - last_wsum, box);
            last_wsum = t_next;
            t_wsum    = t_next + wsum.time_advance();
        }

        // wsum.out → int2.in_u  (ẋ₂)
        if (out_wsum) {
            auto box = make_box<qss1_integrator<TIME>>();
            set_msg<int_in>(box, *out_wsum);
            int2.external_transition(t_next - last_int2, box);
            last_int2 = t_next;
            t_int2    = t_next + int2.time_advance();
        }

        // sensor.out_y → nird.in_y
        if (out_sen) {
            auto box = make_box<nird<TIME, RNG>>();
            set_msg<nd_in>(box, *out_sen);
            nd.external_transition(t_next - last_nd, box, rng);
            last_nd = t_next;
            t_nd    = t_next + nd.time_advance();
        }

        // nird.out_y → hold_ctrl.in_y
        if (out_nd) {
            auto box = make_box<hold_ctrl<TIME>>();
            set_msg<hc_in>(box, *out_nd);
            hc.external_transition(t_next - last_hc, box);
            last_hc = t_next;
            t_hc    = t_next + hc.time_advance();
        }

        // hold_ctrl.out_u → wsum.in<0>  (u = 1 − y_held)
        if (out_hc) {
            auto box = make_box<qss_wsum<2, TIME>>();
            set_msg<ws2_in0>(box, *out_hc);
            wsum.external_transition(t_next - last_wsum, box);
            last_wsum = t_next;
            t_wsum    = t_next + wsum.time_advance();
        }
    }

    // Final MSE contribution from [t_last_y, t_end]
    mse_sum += (1.0 - last_y) * (1.0 - last_y) * (t_end - t_last_y);
    return mse_sum / t_end;
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    bool quick = (argc > 1 && std::string_view(argv[1]) == "--quick");

    const double t_end = quick ? 200.0 : 10000.0;
    const int n_reps   = quick ? 3 : N_REPS;
    const int steps    = quick ? 4 : 8; // τ_max up to 0.4 or 0.8

    std::cout << "CKW10 §7.3 Networked Control System — MSE vs τ_max\n"
              << "  Plant: G_P(s)=1/(s²+0.8s)  h=" << H << "s  v_η=" << V_ETA << "  DQ=" << DQ
              << "  T=" << t_end << "s  N=" << n_reps << (quick ? "  [quick]\n" : "\n") << "\n";

    std::cout << std::fixed << std::setprecision(6);
    std::cout << std::left << std::setw(10) << "tau_max" << std::setw(16) << "MSE_mean"
              << std::setw(16) << "MSE_min" << std::setw(16) << "MSE_max" << "\n";
    std::cout << std::string(58, '-') << "\n";

    const RNG::result_type base_seed = 42u;

    for (int step = 1; step <= steps; ++step) {
        double tau_max = step * 0.1;

        double sum = 0.0, mn = INF, mx = 0.0;
        for (int rep = 0; rep < n_reps; ++rep) {
            double mse = run_once(tau_max, t_end, base_seed + static_cast<RNG::result_type>(rep));
            sum += mse;
            mn = std::min(mn, mse);
            mx = std::max(mx, mse);
        }

        std::cout << std::setw(10) << tau_max << std::setw(16) << (sum / n_reps) << std::setw(16)
                  << mn << std::setw(16) << mx << "\n";
    }

    return 0;
}
