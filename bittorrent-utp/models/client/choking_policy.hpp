// SPDX-License-Identifier: BSD-2-Clause
/**
 * choking_policy: the deterministic-pass BEP 3 ChokingPolicy atomic (C03,
 * "Incentives Build Robustness in BitTorrent"). Ports match
 * stub_always_unchoke's shape (obs_in, choke_cmd_out) but are this
 * atomic's own distinct types — wiring a real ChokingPolicy into
 * bittorrent_client's coupling is bead e332's job, not this one's.
 *
 * Ranks peers by a rolling ~20s data-rate window and unchokes the top 4,
 * plus one round-robin optimistic slot (a deterministic stand-in for
 * BEP 3's random optimistic draw, weighted 3x toward newcomers — here,
 * newcomers simply rotate first). Rechokes every 10s, rotates the
 * optimistic slot every 30s, and marks a peer snubbed if we've gone 60s
 * without receiving anything from them while interested in and unchoked
 * by them (excluding them from the rate-ranked slots; they remain
 * eligible only for the optimistic slot, matching BEP 3's anti-snub rule
 * so a single stalled peer can't monopolize an unchoke slot forever).
 *
 * The ranking metric switches from download-rate to upload-rate once our
 * own download completes (BEP 3's seed behavior) — peer_wire's obs_out
 * carries both obs_completion (download) and obs_upload (upload,
 * peer_wire.hpp) events for exactly this; this atomic derives its own
 * rolling rate from whichever one is currently relevant rather than
 * consuming a pre-computed rate from peer_wire (obs_rx_rate/obs_snub are
 * structurally present in peer_wire_obs but never emitted — see
 * peer_wire.hpp's own doc comment — this atomic's timer-driven logic is
 * exactly what "computing a meaningful rate/snub" needs).
 *
 * Det pass det stand-ins (sto pass replaces both with weighted/uniform
 * random draws — the two protocol-mandated randomness points in this
 * spec): optimistic rotation is strict round-robin (no random draw), and
 * rate/rank ties break on ascending peer_id.
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

    struct choking_policy_defs {
        struct obs_in : public cadmium::in_port<peer_wire_obs> {};
        struct choke_cmd_out : public cadmium::out_port<choke_cmd> {};
    };

    // SimTime (sim_time.hpp): see utp_socket.hpp for why this isn't
    // restricted to std::floating_point.
    template <SimTime TIME> class choking_policy {
      public:
        using defs = choking_policy_defs;

        static constexpr std::uint32_t rate_slots         = 4;
        static constexpr double rate_window_seconds       = 20.0;
        static constexpr double rechoke_period_seconds    = 10.0;
        static constexpr double optimistic_period_seconds = 30.0;
        static constexpr double snub_seconds              = 60.0;

        struct peer_state {
            bool interested{};
            bool unchoked{};
            bool snubbed{};
            std::optional<TIME> last_data{};      // nullopt: never received anything from them
            std::optional<TIME> unchoked_since{}; // nullopt: currently choked
            std::deque<std::pair<TIME, double>> rate_samples{}; // (timestamp, bytes)

            [[nodiscard]] double current_rate(TIME now) const {
                double total = 0.0;
                for (const auto &[t, bytes] : rate_samples) {
                    if (cadmium::log::to_sim_double(now - t) <= rate_window_seconds) {
                        total += bytes;
                    }
                }
                return total / rate_window_seconds;
            }

            friend std::ostream &operator<<(std::ostream &os, const peer_state &p) {
                os << "interested:" << p.interested << " unchoked:" << p.unchoked;
                if (p.snubbed) {
                    os << " SNUBBED";
                }
                return os;
            }
        };

        struct state_type {
            std::uint32_t total_pieces{};
            double sub_piece_bytes{};
            std::map<peer_id, peer_state> peers{};
            std::vector<peer_id> rotation{}; // eligible-order for the optimistic slot
            std::set<std::uint32_t> completed_pieces{};
            bool have_complete_file{};
            std::set<peer_id> rate_unchoked{};
            std::optional<peer_id> optimistic{};
            TIME now{};
            TIME next_rechoke{};
            TIME next_optimistic{};
            std::deque<choke_cmd> pending{};

            friend std::ostream &operator<<(std::ostream &os, const state_type &s) {
                os << "peers:" << s.peers.size() << " rate_unchoked:" << s.rate_unchoked.size();
                os << " optimistic:";
                if (s.optimistic) {
                    os << *s.optimistic;
                } else {
                    os << "-";
                }
                os << (s.have_complete_file ? " metric:upload" : " metric:download");
                os << " pending:" << s.pending.size();
                for (const auto &[peer, p] : s.peers) {
                    os << " [" << peer << ":" << p << "]";
                }
                return os;
            }
        };
        state_type state{};

        /// initial_complete: true for a client that's already a full seed
        /// from construction (matching peer_wire's own initial_have
        /// parameter) -- such a client never downloads anything, so it
        /// would never otherwise observe enough obs_completion events to
        /// ever learn have_complete_file should be true, leaving it
        /// stuck ranking by (nonexistent) download-rate and computing
        /// snub against every peer forever.
        choking_policy(std::uint32_t total_pieces, double sub_piece_bytes,
                       bool initial_complete = false) {
            if (total_pieces == 0) {
                throw std::invalid_argument("choking_policy: total_pieces must be > 0");
            }
            if (!(sub_piece_bytes > 0.0)) {
                throw std::invalid_argument("choking_policy: sub_piece_bytes must be > 0");
            }
            state.total_pieces       = total_pieces;
            state.sub_piece_bytes    = sub_piece_bytes;
            state.have_complete_file = initial_complete;
            state.next_rechoke       = seconds_converter<TIME>::convert(rechoke_period_seconds);
            state.next_optimistic    = seconds_converter<TIME>::convert(optimistic_period_seconds);
        }

        using input_ports  = std::tuple<typename defs::obs_in>;
        using output_ports = std::tuple<typename defs::choke_cmd_out>;

        void external_transition(TIME elapsed,
                                 typename cadmium::make_message_box<input_ports>::type box) {
            state.now       = state.now + elapsed;
            const auto &obs = cadmium::get_message<typename defs::obs_in>(box);
            if (!obs.has_value()) {
                return;
            }
            std::visit(
                [this](const auto &alt) {
                    using T = std::decay_t<decltype(alt)>;
                    if constexpr (std::is_same_v<T, obs_peer_interest>) {
                        ensure_peer(alt.peer).interested = alt.peer_interested;
                    } else if constexpr (std::is_same_v<T, obs_completion>) {
                        auto &p     = ensure_peer(alt.peer);
                        p.last_data = state.now;
                        p.snubbed   = false;
                        if (!state.have_complete_file) {
                            p.rate_samples.emplace_back(state.now, state.sub_piece_bytes);
                        }
                        if (alt.full_piece) {
                            state.completed_pieces.insert(alt.piece_index);
                            if (state.completed_pieces.size() >= state.total_pieces) {
                                state.have_complete_file = true;
                                // Snub no longer applies once we're a
                                // seed (see rechoke()) -- clear any stale
                                // snub marks from just before completion
                                // so they don't wrongly linger into the
                                // upload-rate ranking.
                                for (auto &kv : state.peers) {
                                    kv.second.snubbed = false;
                                }
                            }
                        }
                    } else if constexpr (std::is_same_v<T, obs_upload>) {
                        auto &p = ensure_peer(alt.peer);
                        if (state.have_complete_file) {
                            p.rate_samples.emplace_back(state.now, static_cast<double>(alt.bytes));
                        }
                    } else {
                        // obs_availability / obs_rx_rate / obs_snub: the
                        // first is piece_selector's concern, not this
                        // atomic's; the latter two are structurally
                        // present in peer_wire_obs but never emitted (see
                        // peer_wire.hpp) -- this atomic derives its own
                        // rate/snub from obs_completion/obs_upload
                        // timestamps instead.
                    }
                },
                *obs);
        }

        void internal_transition() {
            // Same two-step timer pattern as utp_socket: a due rechoke/
            // optimistic-rotation timer can legitimately produce *no*
            // command (nothing changed in the ranking) — output() for
            // that step correctly returns empty, and any command the
            // timer *does* produce gets queued here for the following
            // step, not this one.
            const TIME sigma = time_advance();
            if (sigma == std::numeric_limits<TIME>::infinity()) {
                throw std::logic_error("choking_policy::internal_transition called while passive");
            }
            state.now = state.now + sigma;
            if (!state.pending.empty()) {
                state.pending.pop_front();
                return;
            }
            if (!(state.now < state.next_rechoke)) {
                rechoke();
                state.next_rechoke =
                    state.next_rechoke + seconds_converter<TIME>::convert(rechoke_period_seconds);
            }
            if (!(state.now < state.next_optimistic)) {
                rotate_optimistic();
                state.next_optimistic = state.next_optimistic +
                                        seconds_converter<TIME>::convert(optimistic_period_seconds);
            }
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            // Same contract as internal_transition(): imminent means
            // time_advance() == 0, not "pending is non-empty" — a due
            // timer with nothing to report yet is legitimately imminent
            // with an empty box (see internal_transition()'s comment).
            if (time_advance() == std::numeric_limits<TIME>::infinity()) {
                throw std::logic_error("choking_policy::output called while passive");
            }
            typename cadmium::make_message_box<output_ports>::type box;
            if (!state.pending.empty()) {
                cadmium::get_message<typename defs::choke_cmd_out>(box).emplace(
                    state.pending.front());
            }
            return box;
        }

        TIME time_advance() const {
            if (!state.pending.empty()) {
                return TIME{};
            }
            const TIME rechoke_rem =
                state.now < state.next_rechoke ? state.next_rechoke - state.now : TIME{};
            const TIME optimistic_rem =
                state.now < state.next_optimistic ? state.next_optimistic - state.now : TIME{};
            return rechoke_rem < optimistic_rem ? rechoke_rem : optimistic_rem;
        }

      private:
        peer_state &ensure_peer(peer_id peer) {
            auto [it, inserted] = state.peers.try_emplace(peer);
            if (inserted) {
                // Newcomers first (decision: det stand-in for BEP 3's 3x
                // newcomer weighting in the optimistic draw).
                state.rotation.insert(state.rotation.begin(), peer);
            }
            return it->second;
        }

        void set_unchoked(peer_id peer, bool unchoke) {
            auto it        = state.peers.find(peer);
            const bool was = it != state.peers.end() && it->second.unchoked;
            if (was == unchoke) {
                return;
            }
            if (it != state.peers.end()) {
                it->second.unchoked = unchoke;
                // The 60s snub clock has to start from when they were
                // actually *given a chance* (unchoked), not from when we
                // first noticed them (first_seen) -- a peer we've been
                // choking for a long time hasn't had any opportunity to
                // send us anything, and shouldn't read as snubbed the
                // instant we finally unchoke them.
                it->second.unchoked_since = unchoke ? std::optional<TIME>(state.now) : std::nullopt;
            }
            state.pending.push_back(choke_cmd{peer, unchoke});
        }

        void rechoke() {
            // Snub is a download-reciprocity concept: it only makes sense
            // while we're still ranking by download-rate. Once we're a
            // seed (have_complete_file), we no longer expect *any* peer
            // to send us data -- a leech legitimately never does, and
            // computing snub here would eventually mark every one of
            // them snubbed for no real reason.
            if (!state.have_complete_file) {
                for (auto &[peer, p] : state.peers) {
                    if (p.interested && p.unchoked) {
                        // set_unchoked() always sets unchoked_since when
                        // it sets unchoked = true, so this dereference is
                        // safe whenever we reach here.
                        const TIME baseline = p.last_data.value_or(*p.unchoked_since);
                        p.snubbed =
                            cadmium::log::to_sim_double(state.now - baseline) >= snub_seconds;
                    }
                }
            }

            std::vector<peer_id> candidates;
            for (const auto &[peer, p] : state.peers) {
                if (p.interested && !p.snubbed) {
                    candidates.push_back(peer);
                }
            }
            std::sort(candidates.begin(), candidates.end(), [this](peer_id a, peer_id b) {
                const double ra = state.peers.at(a).current_rate(state.now);
                const double rb = state.peers.at(b).current_rate(state.now);
                if (ra != rb) {
                    return ra > rb; // higher rate first
                }
                return a < b; // det tie-break: lowest peer_id
            });
            const std::size_t keep = std::min<std::size_t>(candidates.size(), rate_slots);
            const std::set<peer_id> new_rate_unchoked(
                candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(keep));

            for (peer_id peer : state.rate_unchoked) {
                if (!new_rate_unchoked.contains(peer) &&
                    (!state.optimistic || *state.optimistic != peer)) {
                    set_unchoked(peer, false);
                }
            }
            for (peer_id peer : new_rate_unchoked) {
                if (!state.rate_unchoked.contains(peer)) {
                    set_unchoked(peer, true);
                }
            }
            state.rate_unchoked = new_rate_unchoked;
        }

        void rotate_optimistic() {
            std::vector<peer_id> eligible;
            for (peer_id peer : state.rotation) {
                auto it = state.peers.find(peer);
                if (it != state.peers.end() && it->second.interested &&
                    !state.rate_unchoked.contains(peer)) {
                    eligible.push_back(peer);
                }
            }
            if (eligible.empty()) {
                if (state.optimistic) {
                    // Only actually choke them if the rate-ranked slots
                    // aren't *also* keeping them unchoked (they could have
                    // risen into the top rate_slots since they became the
                    // optimistic pick).
                    if (!state.rate_unchoked.contains(*state.optimistic)) {
                        set_unchoked(*state.optimistic, false);
                    }
                    state.optimistic.reset();
                }
                return;
            }
            std::size_t next_idx = 0;
            if (state.optimistic) {
                auto it = std::find(eligible.begin(), eligible.end(), *state.optimistic);
                if (it != eligible.end()) {
                    next_idx =
                        (static_cast<std::size_t>(it - eligible.begin()) + 1) % eligible.size();
                }
            }
            const peer_id next = eligible[next_idx];
            if (state.optimistic && *state.optimistic != next &&
                !state.rate_unchoked.contains(*state.optimistic)) {
                set_unchoked(*state.optimistic, false);
            }
            state.optimistic = next;
            if (!state.rate_unchoked.contains(next)) {
                set_unchoked(next, true);
            }
        }
    };

} // namespace bt_utp
