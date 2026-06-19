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

/**
 * Trajectory validation for the K04 §4.2 bouncing-ball-downstairs model.
 *
 * A custom spdlog sink intercepts cadmium sim_state events as they are
 * emitted (no file I/O, no post-processing) and tracks the quantized
 * outputs of int_x and int_y.  The test then asserts K04-derived bounds
 * on the trajectory at t=10 s.
 *
 * Physics reference for the bounds (K04 §4.2, eq. 18):
 *   dvx/dt = -0.1*vx  →  vx(t) = 0.5*exp(-0.1*t)
 *   x(t)   = 0.575 + 5*(1 - exp(-0.1*t))
 *   x(10)  ≈ 3.74  (analytical, frictionless x-axis)
 *
 * At x≈3.74 the ball is on stair 4 (height = floor(11-3.74) = 7).
 * Vertical dynamics are complex (spring-damper bounces) so y bounds
 * are loose but exclude the initial position and the ground floor.
 */

#include <cadmium/engine/devs_runner.hpp>
#include <cadmium/logger/cadmium_log.hpp>

#include "ball_model.hpp"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <mutex>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

// Intercepts cadmium sim_state NDJSON lines and records the last quantized
// output (q) of int_x (ball horizontal position) and int_y (ball height).
//
// cadmium's emit() formats each event as NDJSON before passing it to spdlog.
// With set_pattern("%v") the spdlog payload IS the NDJSON line, so we parse
// it directly in sink_it_ without any additional formatting overhead.
//
// NDJSON format (from cadmium::log::emit):
//   {"ts":"...","level":"debug","event":"sim_state",
//    "msg":"int_x x=0.575 u=0.5 q=0.575 dq=0.001 sigma=inf","sim_time":0}
//
// We search for "msg":"int_x " (stable because model_name() = "int_x") then
// parse the " q=" field.  The space prefix distinguishes " q=" from " dq=".
class trajectory_sink : public spdlog::sinks::base_sink<std::mutex> {
  public:
    double last_x{0.575};
    double last_y{10.5};
    double min_y{10.5};

  protected:
    void sink_it_(const spdlog::details::log_msg &msg) override {
        std::string_view line{msg.payload.data(), msg.payload.size()};

        if (line.find("\"event\":\"sim_state\"") == std::string_view::npos)
            return;

        bool is_x = line.find("\"msg\":\"int_x ") != std::string_view::npos;
        bool is_y = line.find("\"msg\":\"int_y ") != std::string_view::npos;
        if (!is_x && !is_y)
            return;

        auto q_pos = line.find(" q=");
        if (q_pos == std::string_view::npos)
            return;
        q_pos += 3; // skip " q="

        // Parse the double value; stop at the first space or closing quote.
        auto end             = line.find_first_of(" \"", q_pos);
        std::string_view tok = line.substr(
            q_pos, end == std::string_view::npos ? std::string_view::npos : end - q_pos);

        // Skip non-finite tokens ("inf", "-inf", "nan") — not relevant here
        // because q values are always finite during the simulation.
        if (tok == "inf" || tok == "-inf" || tok == "nan" || tok.empty())
            return;

        double val = std::stod(std::string(tok));

        if (is_x) {
            last_x = val;
        } else {
            last_y = val;
            min_y  = std::min(min_y, val);
        }
    }

    void flush_() override {}
};

TEST_CASE("K04 §4.2 ball descends staircase over t=[0,10]", "[ball][k04]") {
    auto sink   = std::make_shared<trajectory_sink>();
    auto logger = std::make_shared<spdlog::logger>("cadmium", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::debug);
    cadmium::log::detail::instance() = logger;

    cadmium::engine::devs::runner<TIME, bouncing_ball> r{0.0};
    r.run_until(10.0);
    cadmium::log::flush();

    cadmium::log::detail::instance() = nullptr; // detach sink before assertions

    // Horizontal: analytical frictionless prediction x(10)≈3.74.
    // Allow generous tolerance for QSS1 quantization error.
    CHECK(sink->last_x > 2.5);
    CHECK(sink->last_x < 5.5);

    // Vertical: ball must have descended clearly below the initial height
    // (10.5 m) and remain above the staircase base.
    CHECK(sink->min_y < 10.0); // touched at least the first step
    CHECK(sink->last_y < 9.5); // ended below the starting stair level
    CHECK(sink->last_y > 3.0); // hasn't fallen through the stairs
}
