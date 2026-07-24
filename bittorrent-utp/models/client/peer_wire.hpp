// SPDX-License-Identifier: BSD-2-Clause
/**
 * peer_wire: the BEP 3 per-connection FSM atomic. Deterministic always
 * (decision 0.3: protocol-mandated randomness lives in policy atomics, not
 * here). Multiplexed by peer_id from the start (decision 0.5), even though
 * this stage only ever has one connection — a per-connection *component*
 * would make the swarm model's component count scale with peer count,
 * which breaks compile-time coupling at scale.
 *
 * Ports:
 *   in:  wire_in (utp_socket's app_deliver -> a wire_msg from a peer),
 *        choke_cmd (policy -> {peer, choke|unchoke}),
 *        plan_in (piece-selector -> {peer, list of sub-pieces to fetch})
 *   out: wire_out (a wire_msg to utp_socket's app_send),
 *        obs_out (observation events for the policy atomics — the *only*
 *        way they learn about this connection's state, since they're
 *        separate coupled components: peer-interest changes, have/
 *        bitfield updates, rx-rate samples, sub-piece/piece completions,
 *        snub events). rx-rate and snub are structurally present in
 *        peer_wire_obs for port-shape stability (ports final now, policy
 *        atomics change at stage 4) but this stage's peer_wire never
 *        emits them — computing a meaningful rate/snub needs exactly the
 *        kind of timer-driven logic stage 4's real ChokingPolicy
 *        introduces; the stub policies here don't need it.
 *
 * BEP 3's handshake predates the length-prefixed message stream and isn't
 * a bep3_msg alternative (see bep3.hpp), but both travel over the same
 * uTP byte stream in the real protocol — so utp_socket's PAYLOAD here is
 * wire_msg = variant<handshake, bep3_msg>, not bep3_msg alone.
 *
 * Piece/sub-piece layout is uniform (every piece is sub_pieces_per_piece
 * sub-pieces of sub_piece_bytes each, no ragged last piece) — an
 * engineering simplification valid for this project's synthetic
 * scenarios (piece/sub-piece sizes are chosen to divide evenly), not a
 * general BEP 3 requirement.
 */
#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../../msg/bep3.hpp"
#include "../../msg/peer_id.hpp"
#include "../utp/sim_time.hpp"
#include "../utp/utp_socket.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bt_utp {

    /// A single fixed-size request unit: BEP 3's request/piece messages
    /// address (index, begin-byte, length); peer_wire tracks and plans
    /// work in (piece_index, sub_piece_index) pairs and converts to/from
    /// the wire's byte-offset form at the request/piece boundary.
    struct sub_piece_id {
        std::uint32_t piece_index{};
        std::uint32_t sub_piece_index{};

        friend bool operator==(const sub_piece_id &, const sub_piece_id &) = default;
        friend bool operator<(const sub_piece_id &a, const sub_piece_id &b) {
            return std::tie(a.piece_index, a.sub_piece_index) <
                   std::tie(b.piece_index, b.sub_piece_index);
        }
        friend std::ostream &operator<<(std::ostream &os, const sub_piece_id &s) {
            return os << s.piece_index << "." << s.sub_piece_index;
        }
    };

    using wire_variant = std::variant<handshake, bep3_msg>;

    /// Wraps the variant (rather than aliasing it) for the same reason as
    /// bep3_msg: utp_socket calls payload.wire_size() as a member.
    struct wire_msg : wire_variant {
        using wire_variant::wire_variant;

        friend bool operator==(const wire_msg &, const wire_msg &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
            return std::visit([](const auto &alt) { return alt.wire_size(); }, *this);
        }
    };

    inline std::ostream &operator<<(std::ostream &os, const wire_msg &m) {
        std::visit([&os](const auto &alt) { os << alt; }, m);
        return os;
    }

    /// choke_cmd port message: a choking-policy decision for one peer.
    struct choke_cmd {
        peer_id peer{};
        bool unchoke{};

        friend bool operator==(const choke_cmd &, const choke_cmd &) = default;
        friend std::ostream &operator<<(std::ostream &os, const choke_cmd &c) {
            return os << (c.unchoke ? "UNCHOKE " : "CHOKE ") << c.peer;
        }
    };

    /// plan_in port message: sub-pieces to fetch next from one peer
    /// (appended to peer_wire's own pending-request queue for that peer;
    /// peer_wire fills its pipeline from this queue once unchoked), and/or
    /// sub-pieces to cancel from that peer -- piece_selector's endgame
    /// mode requests every remaining sub-piece from every peer that has
    /// it, and cancels the redundant copies once the first one arrives.
    /// A cancellation still queued (not yet requested) is dropped
    /// silently; one already outstanding gets an actual outgoing CANCEL
    /// wire message (see handle_plan_in).
    struct request_plan {
        peer_id peer{};
        std::vector<sub_piece_id> items{};
        std::vector<sub_piece_id> cancellations{};

        friend bool operator==(const request_plan &, const request_plan &) = default;
        friend std::ostream &operator<<(std::ostream &os, const request_plan &p) {
            return os << "plan->" << p.peer << " [" << p.items.size() << " items, "
                      << p.cancellations.size() << " cancels]";
        }
    };

    struct obs_peer_interest {
        peer_id peer{};
        bool peer_interested{};

        friend bool operator==(const obs_peer_interest &, const obs_peer_interest &) = default;
    };
    struct obs_availability {
        peer_id peer{};
        std::vector<bool> bitfield{}; // full snapshot after any have/bitfield update

        friend bool operator==(const obs_availability &, const obs_availability &) = default;
    };
    struct obs_rx_rate {
        peer_id peer{};
        double bytes_per_sec{};

        friend bool operator==(const obs_rx_rate &, const obs_rx_rate &) = default;
    };
    struct obs_completion {
        peer_id peer{};
        std::uint32_t piece_index{};
        bool full_piece{}; // true: whole piece done (a sub-piece completion always also has a value
                           // here)
        std::uint32_t sub_piece_index{}; // which sub-piece this specific delivery completed --
                                         // piece_selector's endgame mode needs this to know
                                         // exactly what to cancel from the *other* peers it
                                         // redundantly asked for the same sub-piece.

        friend bool operator==(const obs_completion &, const obs_completion &) = default;
    };
    /// Emitted whenever we serve one of *their* requests (the upload
    /// direction) — obs_completion above is download-only (data *we*
    /// received). ChokingPolicy needs both: it ranks peers by download
    /// rate while we're still downloading ourselves, then switches to
    /// upload rate once have_complete_file (BEP 3's seed behavior), and
    /// there is no other signal in this interface for the latter.
    struct obs_upload {
        peer_id peer{};
        std::uint32_t piece_index{};
        std::uint32_t bytes{};

        friend bool operator==(const obs_upload &, const obs_upload &) = default;
    };
    struct obs_snub {
        peer_id peer{};

        friend bool operator==(const obs_snub &, const obs_snub &) = default;
    };

    using peer_wire_obs = std::variant<obs_peer_interest, obs_availability, obs_rx_rate,
                                       obs_completion, obs_upload, obs_snub>;

    inline std::ostream &operator<<(std::ostream &os, const peer_wire_obs &o) {
        std::visit(
            [&os](const auto &alt) {
                using T = std::decay_t<decltype(alt)>;
                if constexpr (std::is_same_v<T, obs_peer_interest>) {
                    os << "OBS interest peer:" << alt.peer << " interested:" << alt.peer_interested;
                } else if constexpr (std::is_same_v<T, obs_availability>) {
                    os << "OBS availability peer:" << alt.peer << " pieces:" << alt.bitfield.size();
                } else if constexpr (std::is_same_v<T, obs_rx_rate>) {
                    os << "OBS rx_rate peer:" << alt.peer << " Bps:" << alt.bytes_per_sec;
                } else if constexpr (std::is_same_v<T, obs_completion>) {
                    os << "OBS completion peer:" << alt.peer << " piece:" << alt.piece_index << "."
                       << alt.sub_piece_index << (alt.full_piece ? " (full)" : " (sub)");
                } else if constexpr (std::is_same_v<T, obs_upload>) {
                    os << "OBS upload peer:" << alt.peer << " piece:" << alt.piece_index
                       << " bytes:" << alt.bytes;
                } else {
                    os << "OBS snub peer:" << alt.peer;
                }
            },
            o);
        return os;
    }

    struct peer_wire_defs {
        using wire_send_req    = typename utp_socket_defs_t<wire_msg>::send_req;
        using wire_deliver_ind = typename utp_socket_defs_t<wire_msg>::deliver_ind;

        struct wire_in : public cadmium::in_port<wire_deliver_ind> {};
        struct choke_cmd_in : public cadmium::in_port<choke_cmd> {};
        struct plan_in : public cadmium::in_port<request_plan> {};
        struct wire_out : public cadmium::out_port<wire_send_req> {};
        struct obs_out : public cadmium::out_port<peer_wire_obs> {};
    };

    // SimTime (sim_time.hpp): see utp_socket.hpp for why this isn't
    // restricted to std::floating_point.
    template <SimTime TIME> class peer_wire {
      public:
        using defs = peer_wire_defs;

        /// C03 (Cohen, "Incentives Build Robustness in BitTorrent"):
        /// keep up to 5 requests outstanding per peer to fill the pipe
        /// without excessive queuing.
        static constexpr std::uint32_t pipeline_depth = 5;

        /// total_pieces/sub_pieces_per_piece/sub_piece_bytes: uniform
        /// swarm piece layout. initial_have: this client's own piece
        /// possession at t=0 (a seed passes a full bitfield, a leech an
        /// all-false one). connect_to: if set, this peer_wire actively
        /// opens toward that peer (emits handshake+bitfield as a one-shot
        /// at t=0); if unset, it passively waits and replies in kind to
        /// whichever peer's handshake arrives first (acceptor role) —
        /// same initiator/acceptor symmetry as utp_socket itself.
        peer_wire(std::uint32_t total_pieces, std::uint32_t sub_pieces_per_piece,
                  std::uint32_t sub_piece_bytes, std::vector<bool> initial_have,
                  std::optional<peer_id> connect_to = std::nullopt)
            : total_pieces_(total_pieces), sub_pieces_per_piece_(sub_pieces_per_piece),
              sub_piece_bytes_(sub_piece_bytes) {
            if (sub_pieces_per_piece_ == 0) {
                throw std::invalid_argument("peer_wire: sub_pieces_per_piece must be > 0");
            }
            if (sub_piece_bytes_ == 0) {
                throw std::invalid_argument("peer_wire: sub_piece_bytes must be > 0");
            }
            if (initial_have.size() != total_pieces_) {
                throw std::invalid_argument("peer_wire: initial_have size must equal total_pieces");
            }
            state.have = std::move(initial_have);
            if (connect_to.has_value()) {
                conn &c = state.conns[*connect_to];
                queue_handshake_and_bitfield(*connect_to);
                c.handshake_sent = true;
            }
        }

        struct conn {
            bool handshake_sent     = false; // we have sent our handshake to this peer
            bool handshake_received = false; // we have received theirs
            bool am_choking         = true;
            bool am_interested      = false;
            bool peer_choking       = true;
            bool peer_interested    = false;
            std::vector<bool> peer_bitfield{};
            /// Sub-pieces received so far for a piece not yet complete.
            std::map<std::uint32_t, std::vector<bool>> in_progress{};
            std::deque<sub_piece_id> outstanding{}; // requested, awaiting `piece`
            std::deque<sub_piece_id> planned{};     // queued by plan_in, not yet requested

            friend std::ostream &operator<<(std::ostream &os, const conn &c) {
                return os << "hs:" << c.handshake_sent << "/" << c.handshake_received << " ("
                          << (c.am_choking ? 'C' : 'c') << (c.am_interested ? 'I' : 'i')
                          << (c.peer_choking ? 'C' : 'c') << (c.peer_interested ? 'I' : 'i')
                          << ") in_flight:" << c.outstanding.size()
                          << " planned:" << c.planned.size();
            }
        };

        struct state_type {
            std::vector<bool> have{};
            std::map<peer_id, conn> conns{};
            std::deque<std::pair<peer_id, wire_msg>> out_wire{};
            std::deque<peer_wire_obs> out_obs{};

            friend std::ostream &operator<<(std::ostream &os, const state_type &s) {
                os << "have:" << std::count(s.have.begin(), s.have.end(), true) << "/"
                   << s.have.size();
                for (const auto &[peer, c] : s.conns) {
                    os << " [" << peer << ": " << c << "]";
                }
                return os;
            }
        };
        state_type state{};

        using input_ports =
            std::tuple<typename defs::wire_in, typename defs::choke_cmd_in, typename defs::plan_in>;
        using output_ports = std::tuple<typename defs::wire_out, typename defs::obs_out>;

        void internal_transition() {
            // A coordinator only calls internal_transition() on an
            // imminent model (time_advance() == 0), which requires at
            // least one of the two output queues non-empty — *either*
            // alone being empty is legitimate (a tick can produce only a
            // wire message, only an observation, or both), but both empty
            // together means the DEVS calling contract was violated, not
            // a state this atomic's own logic can produce.
            if (state.out_wire.empty() && state.out_obs.empty()) {
                throw std::logic_error("peer_wire::internal_transition called while passive");
            }
            if (!state.out_wire.empty()) {
                state.out_wire.pop_front();
            }
            if (!state.out_obs.empty()) {
                state.out_obs.pop_front();
            }
        }

        void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type box) {
            // wire_in first: network state changes (handshake, bitfield,
            // choke/unchoke, piece data) take priority over local policy
            // commands arriving the same instant, same rationale as
            // utp_socket's net_in-before-app_send ordering.
            if (const auto &msg = cadmium::get_message<typename defs::wire_in>(box);
                msg.has_value()) {
                handle_wire_in(msg->src, msg->payload);
            }
            if (const auto &cmd = cadmium::get_message<typename defs::choke_cmd_in>(box);
                cmd.has_value()) {
                handle_choke_cmd(*cmd);
            }
            if (const auto &plan = cadmium::get_message<typename defs::plan_in>(box);
                plan.has_value()) {
                handle_plan_in(*plan);
            }
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            // Same contract as internal_transition(): at least one queue
            // must be non-empty here.
            if (state.out_wire.empty() && state.out_obs.empty()) {
                throw std::logic_error("peer_wire::output called while passive");
            }
            typename cadmium::make_message_box<output_ports>::type box;
            if (!state.out_wire.empty()) {
                const auto &[dst, msg] = state.out_wire.front();
                cadmium::get_message<typename defs::wire_out>(box).emplace(
                    typename defs::wire_send_req{dst, msg});
            }
            if (!state.out_obs.empty()) {
                cadmium::get_message<typename defs::obs_out>(box).emplace(state.out_obs.front());
            }
            return box;
        }

        TIME time_advance() const {
            return (state.out_wire.empty() && state.out_obs.empty())
                       ? std::numeric_limits<TIME>::infinity()
                       : TIME{};
        }

      private:
        void queue_handshake_and_bitfield(peer_id dst) {
            state.out_wire.emplace_back(dst, wire_msg{handshake{}});
            state.out_wire.emplace_back(dst, wire_msg{bep3_msg{bitfield{state.have}}});
        }

        void queue_to(peer_id dst, wire_msg m) {
            state.out_wire.emplace_back(dst, std::move(m));
        }

        [[nodiscard]] bool missing(std::uint32_t piece_index) const {
            return piece_index >= state.have.size() || !state.have[piece_index];
        }

        void recompute_interest(peer_id peer, conn &c) {
            bool should = false;
            for (std::uint32_t i = 0; i < c.peer_bitfield.size(); ++i) {
                if (c.peer_bitfield[i] && missing(i)) {
                    should = true;
                    break;
                }
            }
            if (should != c.am_interested) {
                c.am_interested = should;
                queue_to(peer,
                         wire_msg{should ? bep3_msg{interested{}} : bep3_msg{not_interested{}}});
            }
        }

        void fill_pipeline(peer_id peer, conn &c) {
            if (c.peer_choking) {
                return;
            }
            while (c.outstanding.size() < pipeline_depth && !c.planned.empty()) {
                sub_piece_id sp = c.planned.front();
                c.planned.pop_front();
                c.outstanding.push_back(sp);
                queue_to(peer, wire_msg{bep3_msg{request{sp.piece_index,
                                                         sp.sub_piece_index * sub_piece_bytes_,
                                                         sub_piece_bytes_}}});
            }
        }

        void handle_wire_in(peer_id src, const wire_msg &msg) {
            if (std::holds_alternative<handshake>(msg)) {
                auto it = state.conns.find(src);
                if (it != state.conns.end() && it->second.handshake_received) {
                    return; // illegal: duplicate handshake on an established connection
                }
                conn &c              = state.conns[src]; // creates the conn if acceptor role
                c.handshake_received = true;
                if (!c.handshake_sent) {
                    queue_handshake_and_bitfield(src);
                    c.handshake_sent = true;
                }
                return;
            }

            auto it = state.conns.find(src);
            if (it == state.conns.end() || !it->second.handshake_received) {
                return; // illegal: any other message before a handshake
            }
            conn &c              = it->second;
            const bep3_msg &wire = std::get<bep3_msg>(msg);

            std::visit(
                [&](const auto &alt) {
                    using T = std::decay_t<decltype(alt)>;
                    if constexpr (std::is_same_v<T, keepalive>) {
                        // No BEP 3 state to update: uTP already owns
                        // connection liveness.
                    } else if constexpr (std::is_same_v<T, choke>) {
                        c.peer_choking = true;
                        // Requests in flight are presumed dropped by the
                        // peer on choke; retry once unchoked.
                        while (!c.outstanding.empty()) {
                            c.planned.push_front(c.outstanding.back());
                            c.outstanding.pop_back();
                        }
                    } else if constexpr (std::is_same_v<T, unchoke>) {
                        c.peer_choking = false;
                        fill_pipeline(src, c);
                    } else if constexpr (std::is_same_v<T, interested>) {
                        c.peer_interested = true;
                        state.out_obs.push_back(obs_peer_interest{src, true});
                    } else if constexpr (std::is_same_v<T, not_interested>) {
                        c.peer_interested = false;
                        state.out_obs.push_back(obs_peer_interest{src, false});
                    } else if constexpr (std::is_same_v<T, have>) {
                        if (alt.index >= total_pieces_) {
                            return; // illegal: have for a piece outside the swarm
                        }
                        if (alt.index >= c.peer_bitfield.size()) {
                            c.peer_bitfield.resize(alt.index + 1, false);
                        }
                        c.peer_bitfield[alt.index] = true;
                        recompute_interest(src, c);
                        state.out_obs.push_back(obs_availability{src, c.peer_bitfield});
                    } else if constexpr (std::is_same_v<T, bitfield>) {
                        if (alt.pieces.size() > total_pieces_) {
                            return; // illegal: bitfield covers more pieces than the swarm has
                        }
                        c.peer_bitfield = alt.pieces;
                        recompute_interest(src, c);
                        state.out_obs.push_back(obs_availability{src, c.peer_bitfield});
                    } else if constexpr (std::is_same_v<T, request>) {
                        if (c.am_choking || missing(alt.index)) {
                            return; // illegal: choking, or they asked for a piece we lack
                        }
                        queue_to(src, wire_msg{bep3_msg{piece{alt.index, alt.begin, alt.length}}});
                        state.out_obs.push_back(obs_upload{src, alt.index, alt.length});
                    } else if constexpr (std::is_same_v<T, piece>) {
                        if (alt.index >= total_pieces_) {
                            return; // illegal: piece for an index outside the swarm
                        }
                        if (alt.begin % sub_piece_bytes_ != 0) {
                            return; // illegal: begin isn't a sub-piece boundary
                        }
                        const std::uint32_t sub_index = alt.begin / sub_piece_bytes_;
                        if (sub_index >= sub_pieces_per_piece_) {
                            return; // illegal: out-of-range sub-piece offset
                        }
                        const sub_piece_id sp{alt.index, sub_index};
                        auto req_it = std::find(c.outstanding.begin(), c.outstanding.end(), sp);
                        if (req_it == c.outstanding.end()) {
                            return; // illegal: unsolicited/unmatched piece data
                        }
                        c.outstanding.erase(req_it);
                        auto &bitmap = c.in_progress[alt.index];
                        if (bitmap.empty()) {
                            bitmap.assign(sub_pieces_per_piece_, false);
                        }
                        bitmap[sub_index] = true;
                        const bool piece_done =
                            std::all_of(bitmap.begin(), bitmap.end(), [](bool b) { return b; });
                        if (piece_done) {
                            if (alt.index >= state.have.size()) {
                                state.have.resize(alt.index + 1, false);
                            }
                            state.have[alt.index] = true;
                            c.in_progress.erase(alt.index);
                            queue_to(src, wire_msg{bep3_msg{have{alt.index}}});
                            // Our own have[] just changed — a have/bitfield
                            // delta either way, so interest must be
                            // recomputed toward *every* connection, not
                            // just src: finishing a piece can also make us
                            // no longer interested in a different peer
                            // whose only useful piece was the one we just
                            // acquired. peer_wire is multiplexed by
                            // peer_id from the start (decision 0.5)
                            // specifically so this generalizes past this
                            // stage's single connection.
                            for (auto &[peer, conn_ref] : state.conns) {
                                recompute_interest(peer, conn_ref);
                            }
                        }
                        state.out_obs.push_back(
                            obs_completion{src, alt.index, piece_done, sub_index});
                        fill_pipeline(src, c);
                    } else if constexpr (std::is_same_v<T, cancel>) {
                        // A real client's cancel retracts a request the
                        // *sender* made to *us* and that we haven't served
                        // yet. This peer_wire has no such gap to close:
                        // an incoming `request` is served synchronously,
                        // in the same external_transition call, subject
                        // only to the current choke state — there's no
                        // pending-to-serve queue for cancel to act on.
                        // (c.planned is *our own* outbound request queue
                        // toward this peer, unrelated to what they're
                        // canceling — using it here would be wrong.)
                    }
                },
                wire);
        }

        void handle_choke_cmd(const choke_cmd &cmd) {
            auto it = state.conns.find(cmd.peer);
            if (it == state.conns.end()) {
                return;
            }
            conn &c = it->second;
            if (c.am_choking == cmd.unchoke) {
                c.am_choking = !cmd.unchoke;
                queue_to(cmd.peer, wire_msg{cmd.unchoke ? bep3_msg{unchoke{}} : bep3_msg{choke{}}});
            }
        }

        void handle_plan_in(const request_plan &plan) {
            auto it = state.conns.find(plan.peer);
            if (it == state.conns.end()) {
                return;
            }
            conn &c = it->second;
            for (const auto &sp : plan.cancellations) {
                if (auto planned_it = std::find(c.planned.begin(), c.planned.end(), sp);
                    planned_it != c.planned.end()) {
                    // Never sent yet: drop it silently, nothing to cancel
                    // on the wire.
                    c.planned.erase(planned_it);
                    continue;
                }
                if (auto outstanding_it = std::find(c.outstanding.begin(), c.outstanding.end(), sp);
                    outstanding_it != c.outstanding.end()) {
                    c.outstanding.erase(outstanding_it);
                    queue_to(plan.peer, wire_msg{bep3_msg{cancel{
                                            sp.piece_index, sp.sub_piece_index * sub_piece_bytes_,
                                            sub_piece_bytes_}}});
                }
                // Neither planned nor outstanding: already delivered or
                // never asked of this peer -- nothing to do.
            }
            for (const auto &sp : plan.items) {
                c.planned.push_back(sp);
            }
            fill_pipeline(plan.peer, c);
        }

        std::uint32_t total_pieces_;
        std::uint32_t sub_pieces_per_piece_;
        std::uint32_t sub_piece_bytes_;
    };

} // namespace bt_utp
