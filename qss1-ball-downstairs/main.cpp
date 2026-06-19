// SPDX-License-Identifier: BSD-2-Clause
/**
 * K04 §4.2 Bouncing Ball Downstairs — QSS1 Experiment
 *
 * Replicates example 4.2 from Kofman (SIAM J. Sci. Comput., 2004):
 *   A ball moves in 2-D (x horizontal, y vertical) and bounces
 *   off staircase steps modelled with a spring-damper contact law.
 *
 * ODEs (K04 eq. 18):
 *   ẋ  = vx
 *   v̇x = -(ba/m) * vx
 *   ẏ  = vy
 *   v̇y = -g - (ba/m)*vy - sw*[(b/m)*vy + (k/m)*(y - floor(h+1-x))]
 *
 * where sw = 1 while y ≤ floor(h+1-x)  (ball in contact with a step).
 * Steps are 1 m wide × 1 m tall; h=10 is the height of the first step.
 *
 * Parameters (K04 Table 1):
 *   m=1, k=100000, b=30, ba=0.1, g=9.8, h=10
 *
 * Initial conditions (K04 p. 19):
 *   x(0)=0.575, vx(0)=0.5, y(0)=10.5, vy(0)=0
 *
 * Quantization (K04-compatible for QSS1):
 *   dq_x=0.001, dq_vx=0.01, dq_y=0.0001, dq_vy=0.01, dqrel=0
 *
 * Circuit:
 *   int_vx.q → int_x.u              (vx integrates x)
 *   int_vx.q → deriv_vx.in<0>       (vx → dvx = -0.1*vx)
 *   deriv_vx.out → int_vx.u
 *   int_vy.q → int_y.u              (vy integrates y)
 *   int_x.q  → deriv_vy.in_x
 *   int_y.q  → deriv_vy.in_y
 *   int_vy.q → deriv_vy.in_vy
 *   deriv_vy.out → int_vy.u
 *
 * Output: NDJSON log to stdout (cadmium structured logging).
 *   Extract trajectories with:
 *     grep sim_state output.ndjson | python3 -c "..."
 */

#include <cadmium/basic_model/qss/qss1_integrator.hpp>
#include <cadmium/basic_model/qss/qss_wsum.hpp>
#include <cadmium/engine/devs_engine_helpers.hpp>
#include <cadmium/engine/devs_runner.hpp>
#include <cadmium/logger/cadmium_log.hpp>
#include <cadmium/modeling/coupling.hpp>

#include "ball_vy_deriv.hpp"
#include <string_view>
#include <tuple>

using namespace cadmium::basic_models::qss;

using TIME = double;

// ── Quantization parameters (K04-compatible for QSS1) ────────────────────────

// QSS1 quantization.
// dq_y must exceed the spring deformation δ=mv²/k≈1e-4 m to avoid Zeno
// micro-bouncing at the floor boundary (k=100000 makes contact very stiff).
static constexpr double DQ_X  = 0.001;
static constexpr double DQ_VX = 0.01;
static constexpr double DQ_Y  = 0.001;
static constexpr double DQ_VY = 0.01;

// ── Submodel wrapper types ────────────────────────────────────────────────────
//
// Each logical submodel needs a distinct C++ type so the static coordinator
// can index into the model tuple without collision.

template <typename T> struct int_x : qss1_integrator<T> {
    int_x() : qss1_integrator<T>(0.575, DQ_X, 0.0) {}
};

template <typename T> struct int_vx : qss1_integrator<T> {
    int_vx() : qss1_integrator<T>(0.5, DQ_VX, 0.0) {}
};

template <typename T> struct int_y : qss1_integrator<T> {
    int_y() : qss1_integrator<T>(10.5, DQ_Y, 0.0) {}
};

template <typename T> struct int_vy : qss1_integrator<T> {
    int_vy() : qss1_integrator<T>(0.0, DQ_VY, 0.0) {}
};

// dvx = -ba/m * vx = -0.1 * vx  →  qss_wsum<1> with coefficient -0.1
template <typename T> struct deriv_vx : qss_wsum<1, T> {
    deriv_vx() : qss_wsum<1, T>({-0.1}) {}
};

// dvy = spring-damper contact physics (experiment-specific atom)
template <typename T> struct deriv_vy : ball_vy_deriv<T> {};

// ── Port aliases (time-independent) ──────────────────────────────────────────

using int_out_q = qss1_integrator_defs::out_q;
using int_in_u  = qss1_integrator_defs::in_u;
using ws1_out   = qss_wsum_defs<1>::out;
using ws1_in0   = qss_wsum_defs<1>::template in<0>;
using vy_out    = ball_vy_deriv_defs::out;
using vy_in_x   = ball_vy_deriv_defs::in_x;
using vy_in_y   = ball_vy_deriv_defs::in_y;
using vy_in_vy  = ball_vy_deriv_defs::in_vy;

// ── Coupled model ─────────────────────────────────────────────────────────────

template <typename T> struct bouncing_ball {
    using input_ports               = std::tuple<>;
    using output_ports              = std::tuple<>;
    using external_input_couplings  = std::tuple<>;
    using external_output_couplings = std::tuple<>;
    using select                    = cadmium::engine::devs::first_imminent;

    template <typename U>
    using models = std::tuple<int_x<U>, int_vx<U>, int_y<U>, int_vy<U>, deriv_vx<U>, deriv_vy<U>>;

    using internal_couplings = std::tuple<
        // horizontal axis
        cadmium::modeling::IC<int_vx, int_out_q, int_x, int_in_u>,
        cadmium::modeling::IC<int_vx, int_out_q, deriv_vx, ws1_in0>,
        cadmium::modeling::IC<deriv_vx, ws1_out, int_vx, int_in_u>,
        // vertical axis
        cadmium::modeling::IC<int_vy, int_out_q, int_y, int_in_u>,
        cadmium::modeling::IC<int_x, int_out_q, deriv_vy, vy_in_x>,
        cadmium::modeling::IC<int_y, int_out_q, deriv_vy, vy_in_y>,
        cadmium::modeling::IC<int_vy, int_out_q, deriv_vy, vy_in_vy>,
        cadmium::modeling::IC<deriv_vy, vy_out, int_vy, int_in_u>>;
};

// ── Main ──────────────────────────────────────────────────────────────────────

// Pass --quiet to suppress NDJSON logging (e.g. for CI).
// Without --quiet, full sim_state / sim_output events go to stdout.
int main(int argc, char **argv) {
    bool quiet = (argc > 1 && std::string_view(argv[1]) == "--quiet");
    if (!quiet)
        cadmium::log::init();

    cadmium::engine::devs::runner<TIME, bouncing_ball> r{0.0};
    r.run_until(10.0);

    if (!quiet)
        cadmium::log::flush();
    return 0;
}
