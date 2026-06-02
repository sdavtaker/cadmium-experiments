// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include <concepts>
#include <cstdint>
#include <stdexcept>
#include <tuple>

struct tick_gen_defs {
    struct out : public cadmium::out_port<int> {};
};

template <typename TIME> class tick_gen {
    using defs = tick_gen_defs;

  public:
    using state_type   = int;
    state_type state   = 0;
    using input_ports  = std::tuple<>;
    using output_ports = std::tuple<defs::out>;

    TIME period() const {
        if constexpr (requires { TIME::from_scaled(100); }) {
            return TIME::from_scaled(100);
        } else if constexpr (requires { TIME{1, 10}; }) {
            return TIME{1, 10};
        } else if constexpr (!std::floating_point<TIME>) {
            return TIME{1};
        } else {
            return TIME{1} / TIME{10};
        }
    }

    constexpr tick_gen() noexcept = default;

    void internal_transition() {}

    void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type) {
        throw std::logic_error("tick_gen has no input ports");
    }

    typename cadmium::make_message_box<output_ports>::type output() const {
        typename cadmium::make_message_box<output_ports>::type box;
        cadmium::get_message<defs::out>(box).emplace(1);
        return box;
    }

    TIME time_advance() const {
        return period();
    }
};
