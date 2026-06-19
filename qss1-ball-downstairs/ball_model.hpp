// SPDX-License-Identifier: BSD-2-Clause
/**
 * Copyright (c) 2026-present, Damian Vicino
 * Carleton University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

/**
 * K04 §4.2 Bouncing Ball Downstairs — shared model types.
 *
 * Defines the submodel wrappers, quantization constants, port aliases and
 * coupled-model topology used by both the main binary and the test binary.
 *
 * ODEs (K04 eq. 18):
 *   ẋ  = vx
 *   v̇x = -(ba/m) * vx                    → qss_wsum<1>(-0.1)
 *   ẏ  = vy
 *   v̇y = f(x, y, vy)                     → ball_vy_deriv atom
 *
 * Quantization (Zeno-safe: dq_y > δ=mv²/k≈8e-4 m):
 *   DQ_X=0.001, DQ_VX=0.01, DQ_Y=0.001, DQ_VY=0.01
 */

#include <cadmium/basic_model/qss/qss1_integrator.hpp>
#include <cadmium/basic_model/qss/qss_wsum.hpp>
#include <cadmium/engine/devs_engine_helpers.hpp>
#include <cadmium/modeling/coupling.hpp>

#include "ball_vy_deriv.hpp"
#include <string>
#include <tuple>

using namespace cadmium::basic_models::qss;

using TIME = double;

// ── Quantization parameters ───────────────────────────────────────────────────

inline constexpr double DQ_X  = 0.001;
inline constexpr double DQ_VX = 0.01;
// dq_y must exceed spring deformation δ=mv²/k≈8e-4 m to prevent Zeno
// micro-bouncing at stair boundaries (k=100000 makes contact very stiff).
inline constexpr double DQ_Y  = 0.001;
inline constexpr double DQ_VY = 0.01;

// ── Submodel wrapper types ────────────────────────────────────────────────────
//
// Each logical instance needs a distinct C++ type so the static coordinator
// can index into the model tuple without collision.
// model_name() provides a stable human-readable id used by the log sink.

template <typename T> struct int_x : qss1_integrator<T> {
    int_x() : qss1_integrator<T>(0.575, DQ_X, 0.0) {}
    static std::string model_name() {
        return "int_x";
    }
};

template <typename T> struct int_vx : qss1_integrator<T> {
    int_vx() : qss1_integrator<T>(0.5, DQ_VX, 0.0) {}
    static std::string model_name() {
        return "int_vx";
    }
};

template <typename T> struct int_y : qss1_integrator<T> {
    int_y() : qss1_integrator<T>(10.5, DQ_Y, 0.0) {}
    static std::string model_name() {
        return "int_y";
    }
};

template <typename T> struct int_vy : qss1_integrator<T> {
    int_vy() : qss1_integrator<T>(0.0, DQ_VY, 0.0) {}
    static std::string model_name() {
        return "int_vy";
    }
};

// dvx = -ba/m * vx = -0.1 * vx
template <typename T> struct deriv_vx : qss_wsum<1, T> {
    deriv_vx() : qss_wsum<1, T>({-0.1}) {}
    static std::string model_name() {
        return "deriv_vx";
    }
};

// dvy = spring-damper contact physics
template <typename T> struct deriv_vy : ball_vy_deriv<T> {
    static std::string model_name() {
        return "deriv_vy";
    }
};

// ── Port aliases (time-independent defs structs) ──────────────────────────────

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
        // horizontal axis: vx → x, dvx ← vx, vx ← dvx
        cadmium::modeling::IC<int_vx, int_out_q, int_x, int_in_u>,
        cadmium::modeling::IC<int_vx, int_out_q, deriv_vx, ws1_in0>,
        cadmium::modeling::IC<deriv_vx, ws1_out, int_vx, int_in_u>,
        // vertical axis: vy → y, deriv_vy ← (x, y, vy), vy ← deriv_vy
        cadmium::modeling::IC<int_vy, int_out_q, int_y, int_in_u>,
        cadmium::modeling::IC<int_x, int_out_q, deriv_vy, vy_in_x>,
        cadmium::modeling::IC<int_y, int_out_q, deriv_vy, vy_in_y>,
        cadmium::modeling::IC<int_vy, int_out_q, deriv_vy, vy_in_vy>,
        cadmium::modeling::IC<deriv_vy, vy_out, int_vy, int_in_u>>;
};
