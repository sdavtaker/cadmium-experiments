// SPDX-License-Identifier: BSD-2-Clause
/**
 * stub_sequential_selector: stage-3 PieceSelector placeholder — the
 * bead's "fixed/sequential pattern": plan the lowest-index piece the peer
 * has that we haven't already planned, sub-pieces in ascending order,
 * one piece at a time. Ports/coupling are the final stage-4 shape; only
 * this atomic's trivial strategy gets replaced by a real PieceSelector.
 *
 * Planning happens independently of choke state (peer_wire itself queues
 * planned-but-not-yet-requested sub-pieces and only issues `request`s once
 * unchoked) — so this atomic reacts purely to obs_availability (learns
 * what the peer has) and obs_completion (a piece finished -> plan the
 * next one), never needing a separate "we got unchoked" signal.
 *
 * One piece planned per trigger (not "keep several in flight") is a
 * stage-3 simplification: with a small sub_pieces_per_piece this can
 * under-fill peer_wire's request pipeline between completions — a stage-4
 * PieceSelector that plans further ahead would keep it fuller. Since this
 * project's swarm eventually needs *rarest-first* selection (not
 * sequential) once there are more than two peers, "sequential" is only
 * ever this stage-3 stand-in, never the target algorithm.
 */
#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "peer_wire.hpp"
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <ostream>

namespace bt_utp {

    struct stub_sequential_selector_defs {
        struct obs_in : public cadmium::in_port<peer_wire_obs> {};
        struct plan_out : public cadmium::out_port<request_plan> {};
    };

    // SimTime (sim_time.hpp, transitively via peer_wire.hpp): see
    // utp_socket.hpp for why this isn't restricted to std::floating_point.
    template <SimTime TIME> class stub_sequential_selector {
      public:
        using defs = stub_sequential_selector_defs;

        explicit stub_sequential_selector(std::uint32_t sub_pieces_per_piece)
            : sub_pieces_per_piece_(sub_pieces_per_piece) {}

        struct per_peer {
            std::vector<bool> peer_bitfield{};
            std::uint32_t next_piece_to_plan = 0; // sequential cursor
        };

        struct state_type {
            std::map<peer_id, per_peer> peers{};
            std::deque<request_plan> pending{};

            friend std::ostream &operator<<(std::ostream &os, const state_type &s) {
                return os << "peers:" << s.peers.size() << " pending:" << s.pending.size();
            }
        };
        state_type state{};

        using input_ports  = std::tuple<typename defs::obs_in>;
        using output_ports = std::tuple<typename defs::plan_out>;

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
            if (const auto *avail = std::get_if<obs_availability>(&*obs); avail != nullptr) {
                state.peers[avail->peer].peer_bitfield = avail->bitfield;
                try_plan_next(avail->peer);
            } else if (const auto *done = std::get_if<obs_completion>(&*obs);
                       done != nullptr && done->full_piece) {
                try_plan_next(done->peer);
            }
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            if (!state.pending.empty()) {
                cadmium::get_message<typename defs::plan_out>(box).emplace(state.pending.front());
            }
            return box;
        }

        TIME time_advance() const {
            return state.pending.empty() ? std::numeric_limits<TIME>::infinity() : TIME{};
        }

      private:
        void try_plan_next(peer_id peer) {
            auto it = state.peers.find(peer);
            if (it == state.peers.end()) {
                return;
            }
            per_peer &p = it->second;
            if (p.next_piece_to_plan >= p.peer_bitfield.size() ||
                !p.peer_bitfield[p.next_piece_to_plan]) {
                return; // nothing new the peer has at our current cursor yet
            }
            std::vector<sub_piece_id> items;
            items.reserve(sub_pieces_per_piece_);
            for (std::uint32_t i = 0; i < sub_pieces_per_piece_; ++i) {
                items.push_back({p.next_piece_to_plan, i});
            }
            state.pending.push_back(request_plan{peer, std::move(items)});
            ++p.next_piece_to_plan;
        }

        std::uint32_t sub_pieces_per_piece_;
    };

} // namespace bt_utp
