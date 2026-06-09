// SPDX-License-Identifier: BSD-2-Clause
/**
 * CKW10 §7.2 Load Balancer Model (LBM) — Validation Experiment
 *
 * Replicates Test Scenario 1 from Castro, Kofman, Wainer (SIMULATION 2010):
 *   d_r = 10  tasks/sec (arrival rate λ = d_r)
 *   s_t1 = s_t2 = 0.2 sec  (mean service times, μ₁ = μ₂ = 5)
 *   b_f ∈ {0.0, 0.1, ..., 1.0}  (balancing factor swept)
 *
 * For each b_f, N_REPS simulations are run.  The simulated task loss
 * probabilities and effective throughput are compared against the
 * analytical Erlang B (M/M/1/1) formulas derived in the paper:
 *
 *   ρ_i    = λ_i / μ_i,  λ₁ = b_f · λ,  λ₂ = (1 - b_f) · λ
 *   P_loss_i = ρ_i / (1 + ρ_i)               [Erlang B, m=1]
 *   P_loss   = b_f · P_loss₁ + (1 - b_f) · P_loss₂
 *   λ'       = λ · (1 - P_loss)
 *
 * Simulation is driven by a manual event loop using the STDEVS atomic
 * model API directly.  Each model satisfies cadmium::concepts::stdevs::
 * AtomicModel<M, double, std::mt19937>.
 */

#include "load_generator.hpp"
#include "server.hpp"
#include "weighted_balancer.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>

using namespace ckw10_lbm;
using RNG = std::mt19937;

// ── Experiment parameters ──────────────────────────────────────────────────

static constexpr double D_R   = 10.0; // mean departure rate (λ)
static constexpr double S_T1  = 0.2;  // mean service time S1
static constexpr double S_T2  = 0.2;  // mean service time S2
static constexpr double T_END = 10000.0;
static constexpr int N_REPS   = 15;

// ── Erlang B formula for M/M/1/1 ──────────────────────────────────────────

static double erlang_b(double rho) {
    return rho / (1.0 + rho);
}

// ── One simulation run ─────────────────────────────────────────────────────

struct run_result {
    double lambda_sim;  // simulated arrival rate
    double lambda1_sim; // tasks routed to S1 / T_END
    double lambda2_sim; // tasks routed to S2 / T_END
    double p_loss1;     // 1 - (completed_s1 / routed_s1)
    double p_loss2;
    double p_loss;
    double lambda_prime; // effective throughput (completed / T_END)
};

static run_result run_once(double b_f, RNG::result_type seed) {
    load_generator<double, RNG> lg(D_R);
    weighted_balancer<double, RNG> wb(b_f);
    server<double, RNG> s1(S_T1);
    server<double, RNG> s2(S_T2);

    RNG rng(seed);

    // Scheduled next-event times for each component.
    double t_lg = lg.time_advance();
    double t_wb = std::numeric_limits<double>::infinity();
    double t_s1 = std::numeric_limits<double>::infinity();
    double t_s2 = std::numeric_limits<double>::infinity();

    // Timestamps for elapsed-time calculations in external transitions.
    double last_wb = 0.0;
    double last_s1 = 0.0;
    double last_s2 = 0.0;

    long long n_arrived   = 0;
    long long n_routed_s1 = 0;
    long long n_routed_s2 = 0;
    long long n_done_s1   = 0;
    long long n_done_s2   = 0;

    while (true) {
        double t_next = std::min({t_lg, t_wb, t_s1, t_s2});
        if (t_next >= T_END)
            break;

        if (t_next == t_lg) {
            // ── LG fires: emit task, route to WB ──────────────────────────
            ++n_arrived;
            lg.internal_transition(rng);
            t_lg = t_next + lg.time_advance();

            typename cadmium::make_message_box<weighted_balancer<double, RNG>::input_ports>::type
                wb_box{};
            cadmium::get_message<wb_defs::inp>(wb_box) = 1;
            wb.external_transition(t_next - last_wb, wb_box, rng);
            last_wb = t_next;
            t_wb    = t_next + wb.time_advance(); // ta = 0 → fires immediately

        } else if (t_next == t_wb) {
            // ── WB fires: route task to S1 or S2 ──────────────────────────
            auto wb_out = wb.output();
            wb.internal_transition(rng);
            t_wb = std::numeric_limits<double>::infinity();

            if (cadmium::get_message<wb_defs::out1>(wb_out).has_value()) {
                ++n_routed_s1;
                typename cadmium::make_message_box<server<double, RNG>::input_ports>::type s1_box{};
                cadmium::get_message<server_defs::inp>(s1_box) = 1;
                s1.external_transition(t_next - last_s1, s1_box, rng);
                last_s1 = t_next;
                t_s1    = t_next + s1.time_advance();
            } else {
                ++n_routed_s2;
                typename cadmium::make_message_box<server<double, RNG>::input_ports>::type s2_box{};
                cadmium::get_message<server_defs::inp>(s2_box) = 1;
                s2.external_transition(t_next - last_s2, s2_box, rng);
                last_s2 = t_next;
                t_s2    = t_next + s2.time_advance();
            }

        } else if (t_next == t_s1) {
            // ── S1 fires: task completed ───────────────────────────────────
            ++n_done_s1;
            s1.internal_transition(rng);
            last_s1 = t_next;
            t_s1    = std::numeric_limits<double>::infinity();

        } else {
            // ── S2 fires: task completed ───────────────────────────────────
            ++n_done_s2;
            s2.internal_transition(rng);
            last_s2 = t_next;
            t_s2    = std::numeric_limits<double>::infinity();
        }
    }

    run_result r{};
    r.lambda_sim  = static_cast<double>(n_arrived) / T_END;
    r.lambda1_sim = static_cast<double>(n_routed_s1) / T_END;
    r.lambda2_sim = static_cast<double>(n_routed_s2) / T_END;

    r.p_loss1 = (n_routed_s1 > 0) ? 1.0 - static_cast<double>(n_done_s1) / n_routed_s1 : 0.0;
    r.p_loss2 = (n_routed_s2 > 0) ? 1.0 - static_cast<double>(n_done_s2) / n_routed_s2 : 0.0;

    double total_completed = static_cast<double>(n_done_s1 + n_done_s2);
    r.lambda_prime         = total_completed / T_END;
    r.p_loss = (n_arrived > 0) ? 1.0 - total_completed / static_cast<double>(n_arrived) : 0.0;
    return r;
}

int main() {
    std::cout << "CKW10 Load Balancer Model — Test Scenario 1\n"
              << "  d_r=" << D_R << "  s_t1=" << S_T1 << "  s_t2=" << S_T2 << "  T=" << T_END
              << "  N=" << N_REPS << "\n\n";

    std::cout << std::fixed << std::setprecision(4);
    std::cout << std::left << std::setw(5) << "b_f" << std::setw(14) << "P_loss1_th"
              << std::setw(14) << "P_loss1_sim" << std::setw(14) << "P_loss2_th" << std::setw(14)
              << "P_loss2_sim" << std::setw(12) << "P_loss_th" << std::setw(12) << "P_loss_sim"
              << std::setw(10) << "lam'_th" << std::setw(10) << "lam'_sim" << "\n";
    std::cout << std::string(105, '-') << "\n";

    const RNG::result_type base_seed = 42u;

    for (int bf_step = 0; bf_step <= 10; ++bf_step) {
        double b_f = bf_step / 10.0;

        // Theoretical values (Erlang B)
        double rho1      = b_f * D_R * S_T1;         // b_f * λ / μ₁
        double rho2      = (1.0 - b_f) * D_R * S_T2; // (1-b_f) * λ / μ₂
        double p1_th     = erlang_b(rho1);
        double p2_th     = erlang_b(rho2);
        double ploss_th  = b_f * p1_th + (1.0 - b_f) * p2_th;
        double lambda_th = D_R * (1.0 - ploss_th);

        // Simulation: average over N_REPS
        double sum_p1 = 0, sum_p2 = 0, sum_pl = 0, sum_lp = 0;
        for (int rep = 0; rep < N_REPS; ++rep) {
            auto r = run_once(b_f, base_seed + static_cast<RNG::result_type>(rep));
            sum_p1 += r.p_loss1;
            sum_p2 += r.p_loss2;
            sum_pl += r.p_loss;
            sum_lp += r.lambda_prime;
        }
        double p1_sim     = sum_p1 / N_REPS;
        double p2_sim     = sum_p2 / N_REPS;
        double ploss_sim  = sum_pl / N_REPS;
        double lambda_sim = sum_lp / N_REPS;

        std::cout << std::setw(5) << b_f << std::setw(14) << p1_th << std::setw(14) << p1_sim
                  << std::setw(14) << p2_th << std::setw(14) << p2_sim << std::setw(12) << ploss_th
                  << std::setw(12) << ploss_sim << std::setw(10) << lambda_th << std::setw(10)
                  << lambda_sim << "\n";
    }

    return 0;
}
