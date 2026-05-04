// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cadmium/basic_model/devs/accumulator.hpp>
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include <stdexcept>
#include <tuple>

using acc_defs   = cadmium::basic_models::devs::accumulator_defs<int>;
using reset_tick = acc_defs::reset_tick;

struct reset_gen_defs {
    struct out : public cadmium::out_port<reset_tick> {};
};

template <typename TIME> class reset_gen {
    using defs = reset_gen_defs;

  public:
    using state_type   = int;
    state_type state   = 0;
    using input_ports  = std::tuple<>;
    using output_ports = std::tuple<defs::out>;

    constexpr reset_gen() noexcept = default;

    void internal_transition() {}

    void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type) {
        throw std::logic_error("reset_gen has no input ports");
    }

    typename cadmium::make_message_box<output_ports>::type output() const {
        typename cadmium::make_message_box<output_ports>::type box;
        cadmium::get_message<defs::out>(box).emplace(reset_tick{});
        return box;
    }

    TIME time_advance() const {
        return TIME{1};
    }
};
