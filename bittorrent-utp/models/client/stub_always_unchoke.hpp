// SPDX-License-Identifier: BSD-2-Clause
/**
 * stub_always_unchoke: stage-3 ChokingPolicy placeholder. The bead's whole
 * behavior in one line: a peer telling us they're interested gets
 * unchoked, and nothing else — it never chokes anyone back. Ports and
 * coupling are the FINAL stage-4 shape (decision: "ports final now, only
 * the atomic types get swapped later"); only this trivial body gets
 * replaced by a real ChokingPolicy in stage 4.
 */
#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "peer_wire.hpp"
#include <deque>
#include <limits>
#include <ostream>

namespace bt_utp {

    struct stub_always_unchoke_defs {
        struct obs_in : public cadmium::in_port<peer_wire_obs> {};
        struct choke_cmd_out : public cadmium::out_port<choke_cmd> {};
    };

    // SimTime (sim_time.hpp, transitively via peer_wire.hpp): see
    // utp_socket.hpp for why this isn't restricted to std::floating_point.
    template <SimTime TIME> class stub_always_unchoke {
      public:
        using defs = stub_always_unchoke_defs;

        struct state_type {
            std::deque<choke_cmd> pending{};

            friend std::ostream &operator<<(std::ostream &os, const state_type &s) {
                return os << "pending:" << s.pending.size();
            }
        };
        state_type state{};

        using input_ports  = std::tuple<typename defs::obs_in>;
        using output_ports = std::tuple<typename defs::choke_cmd_out>;

        void internal_transition() {
            if (!state.pending.empty()) {
                state.pending.pop_front();
            }
        }

        void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type box) {
            const auto &obs = cadmium::get_message<typename defs::obs_in>(box);
            if (!obs.has_value()) {
                return;
            }
            if (const auto *interest = std::get_if<obs_peer_interest>(&*obs);
                interest != nullptr && interest->peer_interested) {
                state.pending.push_back(choke_cmd{interest->peer, /*unchoke=*/true});
            }
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            if (!state.pending.empty()) {
                cadmium::get_message<typename defs::choke_cmd_out>(box).emplace(
                    state.pending.front());
            }
            return box;
        }

        TIME time_advance() const {
            return state.pending.empty() ? std::numeric_limits<TIME>::infinity() : TIME{};
        }
    };

} // namespace bt_utp
