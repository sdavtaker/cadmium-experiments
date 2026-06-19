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

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include <cmath>
#include <limits>
#include <ostream>

/**
 * QSS1 static-function atom for the vertical velocity derivative of the
 * K04 bouncing-ball-downstairs model (Kofman 2004, §4.2, eq. 18).
 *
 * Computes:
 *   floor_ht = floor(H + 1 - x)
 *   sw       = (y <= floor_ht) ? 1 : 0
 *   dvy      = -G - (BA/M)*vy - sw*((B/M)*vy + (K/M)*(y - floor_ht))
 *
 * Parameters (compile-time constants matching K04 Table 1):
 *   G=9.8, M=1, K=100000, B=30, BA=0.1, H=10 (first-step height)
 *
 * Fires immediately (sigma=0) on any input update; sigma returns to
 * infinity after the internal transition.  Satisfies devs::AtomicModel.
 */

struct ball_vy_deriv_defs {
    struct in_x : public cadmium::in_port<double> {};
    struct in_y : public cadmium::in_port<double> {};
    struct in_vy : public cadmium::in_port<double> {};
    struct out : public cadmium::out_port<double> {};
};

template <typename TIME> class ball_vy_deriv : public ball_vy_deriv_defs {
  public:
    using in_x  = ball_vy_deriv_defs::in_x;
    using in_y  = ball_vy_deriv_defs::in_y;
    using in_vy = ball_vy_deriv_defs::in_vy;
    using out   = ball_vy_deriv_defs::out;

    using input_ports  = std::tuple<in_x, in_y, in_vy>;
    using output_ports = std::tuple<out>;

    static constexpr double G  = 9.8;
    static constexpr double M  = 1.0;
    static constexpr double K  = 100000.0;
    static constexpr double B  = 30.0;
    static constexpr double BA = 0.1;
    static constexpr double H  = 10.0; // height of first stair step (m)

    struct state_type {
        double x;
        double y;
        double vy;
        TIME sigma;

        friend std::ostream &operator<<(std::ostream &os, const state_type &s) {
            return os << "x=" << s.x << " y=" << s.y << " vy=" << s.vy << " sigma=" << s.sigma;
        }
    };

    // Initialise with K04 initial conditions so the first output is correct
    // even before any external transition arrives.
    state_type state{0.575, 10.5, 0.0, std::numeric_limits<TIME>::infinity()};

    void internal_transition() {
        state.sigma = std::numeric_limits<TIME>::infinity();
    }

    void external_transition(TIME /*e*/, typename cadmium::make_message_box<input_ports>::type mb) {
        auto &mx  = cadmium::get_message<in_x>(mb);
        auto &my  = cadmium::get_message<in_y>(mb);
        auto &mvy = cadmium::get_message<in_vy>(mb);
        if (mx.has_value())
            state.x = mx.value();
        if (my.has_value())
            state.y = my.value();
        if (mvy.has_value())
            state.vy = mvy.value();
        state.sigma = TIME{0};
    }

    typename cadmium::make_message_box<output_ports>::type output() const {
        typename cadmium::make_message_box<output_ports>::type box;
        double floor_ht = std::floor(H + 1.0 - state.x);
        double sw       = (state.y <= floor_ht) ? 1.0 : 0.0;
        double dvy =
            -G - (BA / M) * state.vy - sw * ((B / M) * state.vy + (K / M) * (state.y - floor_ht));
        cadmium::get_message<out>(box).emplace(dvy);
        return box;
    }

    TIME time_advance() const {
        return state.sigma;
    }
};
