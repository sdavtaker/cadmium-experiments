// SPDX-License-Identifier: BSD-2-Clause
#include "reset_gen.hpp"
#include "tick_gen.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace cadmium;

TEST_CASE("tick_gen period", "[tick_gen]") {
    tick_gen<float> g;
    REQUIRE(g.time_advance() == 0.1f);
}

TEST_CASE("tick_gen output", "[tick_gen]") {
    tick_gen<float> g;
    auto box = g.output();
    auto msg = get_message<tick_gen_defs::out>(box);
    REQUIRE(msg.has_value());
    REQUIRE(*msg == 1);
}

TEST_CASE("tick_gen internal_transition is passive", "[tick_gen]") {
    tick_gen<float> g;
    g.internal_transition();
    REQUIRE(g.time_advance() == 0.1f);
}

TEST_CASE("reset_gen period", "[reset_gen]") {
    reset_gen<float> g;
    REQUIRE(g.time_advance() == 1.0f);
}

TEST_CASE("reset_gen output", "[reset_gen]") {
    reset_gen<float> g;
    auto box = g.output();
    auto msg = get_message<reset_gen_defs::out>(box);
    REQUIRE(msg.has_value());
}

TEST_CASE("reset_gen internal_transition is passive", "[reset_gen]") {
    reset_gen<float> g;
    g.internal_transition();
    REQUIRE(g.time_advance() == 1.0f);
}
