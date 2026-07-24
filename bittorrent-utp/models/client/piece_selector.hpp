// SPDX-License-Identifier: BSD-2-Clause
/**
 * piece_selector: the deterministic-pass BEP 3 PieceSelector atomic (C03,
 * "Incentives Build Robustness in BitTorrent"). Ports match
 * stub_sequential_selector's shape (obs_in, plan_out) but are this
 * atomic's own distinct types — wiring a real PieceSelector into
 * bittorrent_client's coupling is bead e332's job, not this one's.
 *
 * Rule order (C03): strict priority (an already-started, incomplete piece
 * always continues before anything new starts) -> rarest-first (lowest
 * peer-availability count, det tie-break: lowest index) -> random-first
 * (det stand-in: lowest available index, used only while we have zero
 * complete pieces of our own — matches real BitTorrent's "don't all pick
 * the same rarest piece on day one" rationale) -> endgame.
 *
 * Endgame is not a separate mode this atomic switches into explicitly —
 * it falls naturally out of the strict-priority tier: once every
 * not-yet-owned piece is already started (no "virgin", never-requested
 * sub-piece remains anywhere — see in_endgame()), the only pieces left to
 * hand a newly-available peer *are* already-started ones, and the
 * per-sub-piece eligibility check simply stops requiring "nobody else has
 * been asked yet" once that point is reached, allowing redundant
 * requests. Cancellation of the losing copies happens via request_plan's
 * cancellations field (peer_wire.hpp) once the first delivery arrives for
 * a sub-piece requested from more than one peer.
 */
#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../utp/sim_time.hpp"
#include "peer_wire.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace bt_utp {

    struct piece_selector_defs {
        struct obs_in : public cadmium::in_port<peer_wire_obs> {};
        struct plan_out : public cadmium::out_port<request_plan> {};
    };

    // SimTime (sim_time.hpp): see utp_socket.hpp for why this isn't
    // restricted to std::floating_point.
    template <SimTime TIME> class piece_selector {
      public:
        using defs = piece_selector_defs;

        /// Per-sub-piece delivery tracking for a piece we've started but
        /// not yet completed (delivery is tracked regardless of *which*
        /// peer delivered it, since that's what "started" means).
        struct piece_progress {
            std::vector<bool> delivered{};
        };

        struct state_type {
            std::uint32_t total_pieces{};
            std::uint32_t sub_pieces_per_piece{};
            std::vector<bool> have{};
            std::map<peer_id, std::vector<bool>> peer_bitfields{};
            std::vector<std::uint32_t> availability{}; // per-piece: how many peers have it
            std::map<std::uint32_t, piece_progress> in_progress_pieces{}; // started, not complete
            std::map<sub_piece_id, std::set<peer_id>> requested_from{};
            std::uint32_t completed_count{};
            std::optional<std::uint32_t> last_chosen{};
            std::deque<request_plan> pending{};

            [[nodiscard]] bool in_endgame() const {
                for (std::uint32_t idx = 0; idx < total_pieces; ++idx) {
                    if (idx < have.size() && have[idx]) {
                        continue;
                    }
                    auto it = in_progress_pieces.find(idx);
                    if (it == in_progress_pieces.end()) {
                        return false; // an unstarted piece remains: not endgame yet
                    }
                    for (std::uint32_t s = 0; s < sub_pieces_per_piece; ++s) {
                        if (it->second.delivered[s]) {
                            continue;
                        }
                        if (!requested_from.contains(sub_piece_id{idx, s})) {
                            return false; // a virgin sub-piece remains: not endgame yet
                        }
                    }
                }
                return true;
            }

            friend std::ostream &operator<<(std::ostream &os, const state_type &s) {
                os << "mode:";
                if (s.in_endgame()) {
                    os << "endgame";
                } else if (s.completed_count == 0) {
                    os << "random_first";
                } else {
                    os << "rarest_first";
                }
                os << " chosen:";
                if (s.last_chosen) {
                    os << *s.last_chosen;
                } else {
                    os << "-";
                }
                os << " have:" << std::count(s.have.begin(), s.have.end(), true) << "/"
                   << s.total_pieces << " started:" << s.in_progress_pieces.size()
                   << " pending:" << s.pending.size();
                return os;
            }
        };
        state_type state{};

        piece_selector(std::uint32_t total_pieces, std::uint32_t sub_pieces_per_piece) {
            if (total_pieces == 0) {
                throw std::invalid_argument("piece_selector: total_pieces must be > 0");
            }
            if (sub_pieces_per_piece == 0) {
                throw std::invalid_argument("piece_selector: sub_pieces_per_piece must be > 0");
            }
            state.total_pieces         = total_pieces;
            state.sub_pieces_per_piece = sub_pieces_per_piece;
            state.have.assign(total_pieces, false);
            state.availability.assign(total_pieces, 0);
        }

        using input_ports  = std::tuple<typename defs::obs_in>;
        using output_ports = std::tuple<typename defs::plan_out>;

        void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type box) {
            const auto &obs = cadmium::get_message<typename defs::obs_in>(box);
            if (!obs.has_value()) {
                return;
            }
            std::visit(
                [this](const auto &alt) {
                    using T = std::decay_t<decltype(alt)>;
                    if constexpr (std::is_same_v<T, obs_availability>) {
                        update_availability(alt.peer, alt.bitfield);
                        try_plan_for(alt.peer);
                    } else if constexpr (std::is_same_v<T, obs_completion>) {
                        handle_completion(alt.peer, alt.piece_index, alt.sub_piece_index,
                                          alt.full_piece);
                        try_plan_for(alt.peer);
                    } else {
                        // obs_peer_interest / obs_upload / obs_rx_rate /
                        // obs_snub: none carry piece-availability
                        // information -- choking_policy's concern, not
                        // this atomic's.
                    }
                },
                *obs);
        }

        void internal_transition() {
            // A coordinator only calls this when time_advance() == 0,
            // which for this atomic (purely event-driven, no timers)
            // means pending is non-empty by construction -- reaching this
            // with an empty queue is a DEVS calling-contract violation.
            if (state.pending.empty()) {
                throw std::logic_error("piece_selector::internal_transition called while passive");
            }
            state.pending.pop_front();
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            if (state.pending.empty()) {
                throw std::logic_error("piece_selector::output called while passive");
            }
            typename cadmium::make_message_box<output_ports>::type box;
            cadmium::get_message<typename defs::plan_out>(box).emplace(state.pending.front());
            return box;
        }

        TIME time_advance() const {
            return state.pending.empty() ? std::numeric_limits<TIME>::infinity() : TIME{};
        }

      private:
        void update_availability(peer_id peer, const std::vector<bool> &bitfield) {
            auto &old = state.peer_bitfields[peer]; // creates an empty (all-missing) entry if new
            for (std::uint32_t i = 0; i < state.total_pieces; ++i) {
                const bool was = i < old.size() && old[i];
                const bool now = i < bitfield.size() && bitfield[i];
                if (!was && now) {
                    ++state.availability[i];
                }
            }
            old = bitfield;
        }

        void handle_completion(peer_id peer, std::uint32_t piece_index, std::uint32_t sub_index,
                               bool full_piece) {
            auto &progress = state.in_progress_pieces[piece_index]; // defensive: create if missing
            if (progress.delivered.empty()) {
                progress.delivered.assign(state.sub_pieces_per_piece, false);
            }
            progress.delivered[sub_index] = true;

            // Cancel this same sub-piece from every *other* peer we'd
            // also asked (endgame's redundant requests, now resolved).
            const sub_piece_id sp{piece_index, sub_index};
            if (auto rf = state.requested_from.find(sp); rf != state.requested_from.end()) {
                for (peer_id other : rf->second) {
                    if (other == peer) {
                        continue;
                    }
                    state.pending.push_back(request_plan{other, {}, {sp}});
                }
                state.requested_from.erase(rf);
            }

            if (full_piece) {
                if (piece_index >= state.have.size()) {
                    state.have.resize(piece_index + 1, false);
                }
                state.have[piece_index] = true;
                ++state.completed_count;
                state.in_progress_pieces.erase(piece_index);
            }
        }

        void try_plan_for(peer_id peer) {
            auto pb_it = state.peer_bitfields.find(peer);
            if (pb_it == state.peer_bitfields.end()) {
                return;
            }
            const auto &peer_bits = pb_it->second;
            const bool endgame    = state.in_endgame();

            // Tier 1 (strict priority): continue an already-started piece
            // this peer has, ahead of starting anything new.
            for (auto &[piece_idx, progress] : state.in_progress_pieces) {
                if (piece_idx >= peer_bits.size() || !peer_bits[piece_idx]) {
                    continue;
                }
                std::vector<sub_piece_id> items;
                for (std::uint32_t s = 0; s < state.sub_pieces_per_piece; ++s) {
                    if (progress.delivered[s]) {
                        continue;
                    }
                    const sub_piece_id sp{piece_idx, s};
                    auto &askers = state.requested_from[sp];
                    if (askers.contains(peer)) {
                        continue; // already asked this peer for it
                    }
                    if (!askers.empty() && !endgame) {
                        continue; // someone else already has this one; not endgame yet
                    }
                    items.push_back(sp);
                }
                if (!items.empty()) {
                    for (const auto &sp : items) {
                        state.requested_from[sp].insert(peer);
                    }
                    state.last_chosen = piece_idx;
                    state.pending.push_back(request_plan{peer, std::move(items)});
                    return;
                }
            }

            // Tier 2: rarest-first (or random-first while we have zero
            // complete pieces) among pieces not yet started.
            std::optional<std::uint32_t> best;
            for (std::uint32_t idx = 0; idx < state.total_pieces; ++idx) {
                if ((idx < state.have.size() && state.have[idx]) ||
                    state.in_progress_pieces.contains(idx)) {
                    continue;
                }
                if (idx >= peer_bits.size() || !peer_bits[idx]) {
                    continue;
                }
                if (!best) {
                    best = idx;
                    if (state.completed_count == 0) {
                        break; // random-first det stand-in: lowest index, done
                    }
                    continue;
                }
                if (state.completed_count > 0 &&
                    state.availability[idx] < state.availability[*best]) {
                    best = idx; // rarest-first; ascending scan order ties to lowest index
                }
            }
            if (!best) {
                return; // this peer has nothing new (and nothing started) to offer
            }

            auto &progress = state.in_progress_pieces[*best];
            progress.delivered.assign(state.sub_pieces_per_piece, false);
            std::vector<sub_piece_id> items;
            items.reserve(state.sub_pieces_per_piece);
            for (std::uint32_t s = 0; s < state.sub_pieces_per_piece; ++s) {
                const sub_piece_id sp{*best, s};
                state.requested_from[sp].insert(peer);
                items.push_back(sp);
            }
            state.last_chosen = *best;
            state.pending.push_back(request_plan{peer, std::move(items)});
        }
    };

} // namespace bt_utp
