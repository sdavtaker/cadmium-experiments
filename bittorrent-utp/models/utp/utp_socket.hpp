// SPDX-License-Identifier: BSD-2-Clause
/**
 * uTP endpoint atomic: BEP 29 connection lifecycle, packet-based reliable
 * stream with selective ACK, and LEDBAT congestion control — fully
 * deterministic (classic DEVS in both experiment passes; transport
 * randomness lives in the channel models).
 *
 * Multiplexed: per-connection state is keyed by remote peer id, one uTP
 * connection per peer pair, with real connection_id header semantics
 * (initiator picks recv id, send id = recv id + 1).
 *
 * BEP 29 gaps are resolved per the libtorrent reference implementation:
 * gain 3000 B/RTT applied as acked_bytes/in_flight window factor, slow
 * start with ssthres exit on delay >= target or loss, immediate ACKs (no
 * delayed-ACK timer), loss multiplier 0.5, timeout floor 500 ms / initial
 * 1 s with doubling.
 *
 * Deterministic stand-ins for spec-mandated randomness (connection ids and
 * the acceptor's initial seq_nr are random per BEP 29): a per-socket
 * counter and a fixed constant. Both are behavior-neutral here — no
 * competing id spaces exist inside one simulated pair/swarm.
 */
#pragma once

#include <cadmium/logger/cadmium_log.hpp>
#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../../msg/utp_frame.hpp"
#include "sim_time.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <ostream>
#include <tuple>
#include <utility>
#include <vector>

namespace bt_utp {

    template <typename PAYLOAD> struct utp_socket_defs_t {
        using frame_t = utp_frame<PAYLOAD>;

        struct send_req {
            peer_id dst{};
            PAYLOAD payload{};

            friend std::ostream &operator<<(std::ostream &os, const send_req &r) {
                return os << "send->" << r.dst << " " << r.payload;
            }
        };

        struct deliver_ind {
            peer_id src{};
            PAYLOAD payload{};

            friend std::ostream &operator<<(std::ostream &os, const deliver_ind &d) {
                return os << "deliver<-" << d.src << " " << d.payload;
            }
        };

        struct net_in : public cadmium::in_port<frame_t> {};
        struct app_send : public cadmium::in_port<send_req> {};
        struct net_out : public cadmium::out_port<frame_t> {};
        struct app_deliver : public cadmium::out_port<deliver_ind> {};
    };

    /// BEP 29 / libtorrent protocol constants (seconds and bytes).
    struct utp_constants {
        double target_delay         = 0.100; // CCONTROL_TARGET
        double gain_bytes_per_rtt   = 3000.0;
        double base_delay_window    = 120.0;
        double min_timeout          = 0.500;
        double initial_timeout      = 1.000;
        double loss_multiplier      = 0.5;
        std::uint64_t min_packet    = 150;
        std::uint64_t initial_cwnd  = 150; // starting window, distinct from the floor
        std::uint64_t mtu_payload   = 980; // + 20 B header = 1000 B wire
        std::uint64_t recv_window   = 1 << 20;
        std::uint16_t acceptor_seq0 = 1000; // det stand-in for rand()
        int dup_ack_threshold       = 3;
    };

    // SimTime (sim_time.hpp): double by default, but not restricted to
    // std::floating_point — exact-arithmetic TIME types (e.g.
    // cdcommons::time::decimal) must be usable too, since DEVS causality is
    // only exact under exact arithmetic (source-VDW14-devs-time-datatype.md).
    //
    // TIME is exclusively the scheduling clock (state.now, last_activity,
    // time_advance()). Model-internal quantities that merely happen to be
    // duration-like — rtt, rtt_var, timeout (an EWMA over measured samples,
    // inherently approximate) — are NOT TIME: they use the model's own
    // natural representation (double, matching how a real uTP
    // implementation computes them), same as utp_constants' own config
    // values. decimal has no operator/ and division is rarely exact
    // regardless, so it deliberately doesn't offer one; simulator/engine
    // operations never need to divide a TIME value, only model-internal
    // arithmetic does, and that arithmetic isn't TIME to begin with.
    // seconds_converter<TIME> (a double duration -> TIME) and
    // cadmium::log::to_sim_double (TIME -> a double duration) are the two
    // explicit conversions at the one boundary this crosses: scheduling an
    // RTO deadline from c.timeout (double) against c.last_activity (TIME),
    // and measuring a sample RTT from state.now - sent_at (TIME) back into
    // update_rtt's own double arithmetic.
    template <SimTime TIME, typename PAYLOAD> class utp_socket {
      public:
        using defs        = utp_socket_defs_t<PAYLOAD>;
        using frame_t     = typename defs::frame_t;
        using send_req    = typename defs::send_req;
        using deliver_ind = typename defs::deliver_ind;

        utp_socket() = default;
        utp_socket(peer_id self, utp_constants k) : self_(self), k_(k) {}

        enum class conn_state : std::uint8_t {
            syn_sent,
            syn_recv,
            connected,
            fin_sent,
            closed,
        };

        friend std::ostream &operator<<(std::ostream &os, conn_state s) {
            switch (s) {
            case conn_state::syn_sent:
                return os << "SYN_SENT";
            case conn_state::syn_recv:
                return os << "SYN_RECV";
            case conn_state::connected:
                return os << "CONNECTED";
            case conn_state::fin_sent:
                return os << "FIN_SENT";
            case conn_state::closed:
                return os << "CLOSED";
            }
            return os << "?";
        }

        struct inflight_pkt {
            std::uint16_t seq{};
            std::uint64_t wire_bytes{};
            TIME sent_at{};
            int transmissions = 1;
            int sacked_past   = 0; // packets acked past this one (loss signal)
            std::vector<PAYLOAD> completing{};
            std::uint64_t stream_bytes{}; // payload bytes carried
        };

        struct pending_payload {
            PAYLOAD payload{};
            std::uint64_t remaining{};
        };

        struct recv_pkt {
            std::uint64_t stream_bytes{};
            std::vector<PAYLOAD> completing{};
        };

        struct conn {
            conn_state state = conn_state::syn_sent;
            bool initiator   = true;
            std::uint16_t conn_id_send{};
            std::uint16_t conn_id_recv{};
            std::uint16_t seq_nr{}; // next seq to send
            std::uint16_t ack_nr{}; // last in-order seq received
            std::uint32_t reply_micro{};
            std::uint64_t adv_wnd = std::numeric_limits<std::uint32_t>::max();

            std::deque<pending_payload> pending{};
            std::deque<inflight_pkt> inflight{};
            std::map<std::uint16_t, recv_pkt> ooo{}; // out-of-order recv buffer
            bool has_eof = false;
            std::uint16_t eof_pkt{};

            double cwnd              = 0.0; // bytes
            bool slow_start          = true;
            double ssthres           = 0.0;
            double rtt               = 0.0; // model-internal EWMA state, not
            double rtt_var           = 0.0; // TIME — see the class-level
            double timeout           = 0.0; // doc comment above.
            int consecutive_timeouts = 0;
            TIME last_activity{};
            std::uint16_t last_ack_seen{};
            int dup_acks          = 0;
            std::uint16_t cut_seq = 0; // newest seq at last window cut
            bool cut_valid        = false;

            // (bucket_index, min_delay_seconds) minute buckets for base delay
            std::deque<std::pair<std::int64_t, double>> base_hist{};
        };

        struct state_type {
            TIME now{};
            std::map<peer_id, conn> conns{};
            std::deque<frame_t> out_frames{};
            std::deque<deliver_ind> out_delivers{};
            std::uint16_t next_conn_id = 1; // det stand-in for rand()
            std::uint64_t sent_packets = 0, recv_packets = 0, retransmits = 0, timeouts = 0;

            friend std::ostream &operator<<(std::ostream &os, const state_type &s) {
                os << "t:" << s.now << " conns:" << s.conns.size();
                for (const auto &[peer, c] : s.conns) {
                    os << " [" << peer << ":" << c.state
                       << " cwnd:" << static_cast<std::uint64_t>(c.cwnd)
                       << " inflight:" << c.inflight.size() << " pending:" << c.pending.size()
                       << " seq:" << c.seq_nr << " ack:" << c.ack_nr << (c.slow_start ? " ss" : "")
                       << "]";
                }
                return os << " tx:" << s.sent_packets << " rx:" << s.recv_packets
                          << " retx:" << s.retransmits << " to:" << s.timeouts;
            }
        };
        state_type state{};

        using input_ports  = std::tuple<typename defs::net_in, typename defs::app_send>;
        using output_ports = std::tuple<typename defs::net_out, typename defs::app_deliver>;

        // ------------------------------------------------------------------
        // DEVS interface
        // ------------------------------------------------------------------

        TIME time_advance() const {
            if (!state.out_frames.empty() || !state.out_delivers.empty()) {
                return TIME{};
            }
            TIME best = std::numeric_limits<TIME>::infinity();
            for (const auto &[peer, c] : state.conns) {
                if (timer_armed(c)) {
                    const TIME deadline =
                        c.last_activity + seconds_converter<TIME>::convert(c.timeout);
                    const TIME rem = deadline > state.now ? deadline - state.now : TIME{};
                    best           = rem < best ? rem : best;
                }
            }
            return best;
        }

        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            if (!state.out_frames.empty()) {
                cadmium::get_message<typename defs::net_out>(box).emplace(state.out_frames.front());
            }
            if (!state.out_delivers.empty()) {
                cadmium::get_message<typename defs::app_deliver>(box).emplace(
                    state.out_delivers.front());
            }
            return box;
        }

        void internal_transition() {
            const TIME sigma   = time_advance();
            state.now          = state.now + sigma;
            const bool emitted = !state.out_frames.empty() || !state.out_delivers.empty();
            if (!state.out_frames.empty()) {
                state.out_frames.pop_front();
            }
            if (!state.out_delivers.empty()) {
                state.out_delivers.pop_front();
            }
            if (emitted) {
                return;
            }
            // Timer expiry: retransmit on the (a) due connection(s).
            for (auto &[peer, c] : state.conns) {
                if (timer_armed(c) &&
                    !(c.last_activity + seconds_converter<TIME>::convert(c.timeout) > state.now)) {
                    on_timeout(peer, c);
                }
            }
        }

        void external_transition(TIME elapsed,
                                 typename cadmium::make_message_box<input_ports>::type box) {
            state.now = state.now + elapsed;
            // net_in before app_send: if a peer's SYN arrives in the same
            // instant as an app_send targeting that same peer (simultaneous
            // open), accept_syn() must establish the acceptor-role
            // connection first so app_send finds and queues into it,
            // instead of app_send opening its own initiator connection
            // (queuing an outgoing SYN) that accept_syn then immediately
            // erases/replaces, orphaning that queued SYN.
            if (const auto &frm = cadmium::get_message<typename defs::net_in>(box);
                frm.has_value()) {
                handle_frame(*frm);
            }
            if (const auto &req = cadmium::get_message<typename defs::app_send>(box);
                req.has_value()) {
                handle_app_send(*req);
            }
        }

        // ------------------------------------------------------------------
        // Introspection helpers for tests/experiments (read-only)
        // ------------------------------------------------------------------
        const conn *connection(peer_id peer) const {
            const auto it = state.conns.find(peer);
            return it == state.conns.end() ? nullptr : &it->second;
        }

        // Wrap-aware signed distance between two 16-bit sequence numbers.
        // The unsigned-to-signed conversion below is well-defined modulo-2^16
        // two's-complement wraparound as of C++20 ([conv.integral]), which
        // this project requires (C++23) — not implementation-defined here.
        static std::int16_t seq_diff(std::uint16_t a, std::uint16_t b) {
            return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b));
        }

      private:
        static bool timer_armed(const conn &c) {
            return !c.inflight.empty() || c.state == conn_state::syn_sent;
        }

        std::uint32_t now_micros() const {
            return static_cast<std::uint32_t>(static_cast<std::uint64_t>(
                std::llround(cadmium::log::to_sim_double(state.now) * 1e6)));
        }

        utp_packet header_for(const conn &c, packet_type t) const {
            utp_packet p{};
            p.type                              = t;
            p.connection_id                     = c.conn_id_send;
            p.timestamp_microseconds            = now_micros();
            p.timestamp_difference_microseconds = c.reply_micro;
            p.wnd_size                          = static_cast<std::uint32_t>(k_.recv_window);
            p.ack_nr                            = c.ack_nr;
            return p;
        }

        void queue_frame(peer_id dst, utp_packet pkt, std::vector<PAYLOAD> completing = {}) {
            frame_t f{};
            f.src        = self_;
            f.dst        = dst;
            f.pkt        = pkt;
            f.completing = std::move(completing);
            state.out_frames.push_back(std::move(f));
            ++state.sent_packets;
        }

        // -------------------- app side --------------------

        void handle_app_send(const send_req &req) {
            auto it = state.conns.find(req.dst);
            if (it == state.conns.end() || it->second.state == conn_state::closed) {
                // No connection, or the prior one is closed (FIN/RESET): a
                // closed entry never re-opens on its own (packetize only
                // runs while CONNECTED), so start a fresh connection rather
                // than silently dropping the send into a dead entry.
                if (it != state.conns.end()) {
                    state.conns.erase(it);
                }
                it = open_initiator(req.dst);
            }
            conn &c = it->second;
            c.pending.push_back({req.payload, req.payload.wire_size()});
            if (c.state == conn_state::connected) {
                packetize(req.dst, c);
            }
        }

        typename std::map<peer_id, conn>::iterator open_initiator(peer_id dst) {
            conn c{};
            c.state         = conn_state::syn_sent;
            c.initiator     = true;
            c.conn_id_recv  = state.next_conn_id++;
            c.conn_id_send  = static_cast<std::uint16_t>(c.conn_id_recv + 1);
            c.seq_nr        = 1;
            c.cwnd          = static_cast<double>(k_.initial_cwnd);
            c.timeout       = k_.initial_timeout;
            c.last_activity = state.now;

            utp_packet syn{};
            syn.type                   = packet_type::st_syn;
            syn.connection_id          = c.conn_id_recv; // SYN carries the recv id
            syn.timestamp_microseconds = now_micros();
            syn.wnd_size               = static_cast<std::uint32_t>(k_.recv_window);
            syn.seq_nr                 = c.seq_nr++;
            queue_frame(dst, syn);

            return state.conns.emplace(dst, std::move(c)).first;
        }

        void packetize(peer_id dst, conn &c) {
            while (!c.pending.empty()) {
                const std::uint64_t inflight_bytes = bytes_in_flight(c);
                const std::uint64_t budget =
                    std::min<std::uint64_t>(static_cast<std::uint64_t>(c.cwnd), c.adv_wnd);
                std::uint64_t capacity = std::min<std::uint64_t>(k_.mtu_payload, [&] {
                    std::uint64_t room = 0;
                    if (budget > inflight_bytes + utp_header_bytes) {
                        room = budget - inflight_bytes - utp_header_bytes;
                    }
                    return room;
                }());
                if (capacity == 0) {
                    return;
                }
                inflight_pkt ip{};
                ip.seq     = c.seq_nr;
                ip.sent_at = state.now;
                std::vector<PAYLOAD> completing;
                while (capacity > 0 && !c.pending.empty()) {
                    auto &front             = c.pending.front();
                    const std::uint64_t eat = std::min<std::uint64_t>(capacity, front.remaining);
                    front.remaining -= eat;
                    ip.stream_bytes += eat;
                    capacity -= eat;
                    if (front.remaining == 0) {
                        completing.push_back(front.payload);
                        c.pending.pop_front();
                    }
                }
                utp_packet pkt   = header_for(c, packet_type::st_data);
                pkt.seq_nr       = c.seq_nr++;
                pkt.payload_size = ip.stream_bytes;
                ip.wire_bytes    = pkt.wire_size();
                ip.completing    = completing;
                c.inflight.push_back(ip);
                c.last_activity = state.now;
                queue_frame(dst, pkt, std::move(completing));
            }
        }

        static std::uint64_t bytes_in_flight(const conn &c) {
            std::uint64_t b = 0;
            for (const auto &p : c.inflight) {
                b += p.wire_bytes;
            }
            return b;
        }

        // -------------------- network side --------------------

        void handle_frame(const frame_t &f) {
            ++state.recv_packets;
            const utp_packet &pkt = f.pkt;
            if (pkt.type == packet_type::st_syn) {
                accept_syn(f);
                return;
            }
            auto it = state.conns.find(f.src);
            // A packet whose connection_id doesn't match what this socket
            // expects to receive on (our conn_id_recv, per the SYN exchange
            // in open_initiator/accept_syn) is stale or mis-tagged — treat
            // it exactly like an unknown connection rather than accepting it
            // into a live one (BEP 29: connection_id disambiguates streams).
            // ST_RESET is an exception: a peer rejecting a packet it doesn't
            // recognize constructs its reset by echoing back the
            // connection_id it saw on *our* packet (see the reject branch
            // below, which does exactly that) — that echoed value is our
            // conn_id_send, not our conn_id_recv, so a reset matching either
            // of our ids must be accepted or teardown can never propagate.
            const bool known_and_matching =
                it != state.conns.end() && (pkt.connection_id == it->second.conn_id_recv ||
                                            (pkt.type == packet_type::st_reset &&
                                             pkt.connection_id == it->second.conn_id_send));
            if (!known_and_matching) {
                // Stale/foreign packet: reset per BEP 29.
                if (pkt.type != packet_type::st_reset) {
                    utp_packet rst{};
                    rst.type          = packet_type::st_reset;
                    rst.connection_id = pkt.connection_id;
                    rst.ack_nr        = pkt.seq_nr;
                    queue_frame(f.src, rst);
                }
                return;
            }
            conn &c         = it->second;
            c.adv_wnd       = pkt.wnd_size;
            c.last_activity = state.now;
            // One-way delay measurement of the peer->self direction, echoed
            // back on our next packet (BEP 29 reply_micro). Every packet we
            // send carries a timestamp (header_for sets it unconditionally),
            // including ST_STATE, so the sample must be taken from every
            // received type, not just data-bearing ones.
            c.reply_micro = now_micros() - pkt.timestamp_microseconds;

            switch (pkt.type) {
            case packet_type::st_state:
                if (c.state == conn_state::syn_sent) {
                    c.state = conn_state::connected;
                    // Acceptor's ST_STATE carries its next seq; its first
                    // data packet uses that value, so ack one behind it
                    // (libtorrent semantics — ST_STATE consumes no seq).
                    c.ack_nr               = static_cast<std::uint16_t>(pkt.seq_nr - 1);
                    c.consecutive_timeouts = 0;
                    c.timeout              = k_.initial_timeout;
                    packetize(f.src, c);
                    return;
                }
                process_acks(f.src, c, pkt);
                return;
            case packet_type::st_data:
                if (c.state == conn_state::syn_recv) {
                    c.state = conn_state::connected;
                    // Flush any payload the app queued via app_send() while
                    // still SYN_RECV: handle_app_send only packetizes when
                    // already CONNECTED, and process_acks below won't reach
                    // its own packetize() call either (nothing is inflight
                    // yet to be acked), so without this the pending queue
                    // would sit stuck indefinitely.
                    packetize(f.src, c);
                }
                process_acks(f.src, c, pkt);
                receive_data(f.src, c, f);
                return;
            case packet_type::st_fin:
                process_acks(f.src, c, pkt);
                c.has_eof = true;
                c.eof_pkt = pkt.seq_nr;
                receive_data(f.src, c, f); // FIN consumes a seq slot
                return;
            case packet_type::st_reset:
                c.state = conn_state::closed;
                c.inflight.clear();
                c.pending.clear();
                return;
            case packet_type::st_syn:
                return; // handled above
            }
        }

        void accept_syn(const frame_t &f) {
            auto it = state.conns.find(f.src);
            // A SYN is only a retransmit of the CURRENT attempt if we have a
            // live (non-closed) entry using the same connection_id. A closed
            // entry, or a SYN carrying a different connection_id (the peer
            // starting a fresh stream — e.g. after it restarted or reset),
            // must not reuse the stale ids: doing so would ACK the new
            // stream with the old identity and break the handshake.
            const bool is_retransmit_of_current = it != state.conns.end() &&
                                                  it->second.state != conn_state::closed &&
                                                  it->second.conn_id_send == f.pkt.connection_id;
            if (!is_retransmit_of_current) {
                if (it != state.conns.end()) {
                    state.conns.erase(it);
                }
                conn c{};
                c.state         = conn_state::syn_recv;
                c.initiator     = false;
                c.conn_id_send  = f.pkt.connection_id;
                c.conn_id_recv  = static_cast<std::uint16_t>(f.pkt.connection_id + 1);
                c.seq_nr        = k_.acceptor_seq0;
                c.ack_nr        = f.pkt.seq_nr;
                c.adv_wnd       = f.pkt.wnd_size;
                c.cwnd          = static_cast<double>(k_.initial_cwnd);
                c.timeout       = k_.initial_timeout;
                c.last_activity = state.now;
                c.reply_micro   = now_micros() - f.pkt.timestamp_microseconds;
                it              = state.conns.emplace(f.src, std::move(c)).first;
            }
            // ACK the SYN (also on duplicate SYN: the first ST_STATE may be lost).
            conn &c        = it->second;
            utp_packet ack = header_for(c, packet_type::st_state);
            ack.seq_nr     = c.seq_nr; // ST_STATE does not consume a seq
            queue_frame(f.src, ack);
        }

        void receive_data(peer_id src, conn &c, const frame_t &f) {
            const utp_packet &pkt  = f.pkt;
            const std::int16_t rel = seq_diff(pkt.seq_nr, c.ack_nr);
            if (rel <= 0) {
                send_ack(src, c); // duplicate: re-ack
                return;
            }
            if (rel == 1) {
                c.ack_nr = pkt.seq_nr;
                deliver(src, f.completing);
                drain_ooo(src, c);
            } else {
                c.ooo.emplace(pkt.seq_nr, recv_pkt{pkt.payload_size, f.completing});
            }
            send_ack(src, c);
        }

        void drain_ooo(peer_id src, conn &c) {
            for (auto it = c.ooo.find(static_cast<std::uint16_t>(c.ack_nr + 1)); it != c.ooo.end();
                 it      = c.ooo.find(static_cast<std::uint16_t>(c.ack_nr + 1))) {
                c.ack_nr = it->first;
                deliver(src, it->second.completing);
                c.ooo.erase(it);
            }
            if (c.has_eof && seq_diff(c.eof_pkt, c.ack_nr) <= 0) {
                c.state = conn_state::closed;
            }
        }

        void deliver(peer_id src, const std::vector<PAYLOAD> &completing) {
            for (const auto &p : completing) {
                state.out_delivers.push_back({src, p});
            }
        }

        void send_ack(peer_id dst, conn &c) {
            utp_packet ack = header_for(c, packet_type::st_state);
            ack.seq_nr     = c.seq_nr; // no seq consumed
            if (!c.ooo.empty()) {
                ack.sack_mask = build_sack(c);
            }
            queue_frame(dst, ack);
        }

        std::vector<std::uint8_t> build_sack(const conn &c) const {
            // Bits ack seq ack_nr+2, +3, ... (BEP 29: first bit is ack_nr+2,
            // ack_nr+1 is implied missing). Reverse byte order within bytes
            // is a serialization concern; the model stores logical bit i =
            // bit (i%8) of byte (i/8).
            std::vector<std::uint8_t> mask(4, 0);
            for (const auto &[seq, rp] : c.ooo) {
                const std::int16_t off = seq_diff(seq, static_cast<std::uint16_t>(c.ack_nr + 2));
                if (off < 0) {
                    continue;
                }
                const std::size_t bit  = static_cast<std::size_t>(off);
                const std::size_t byte = bit / 8;
                while (mask.size() <= byte) {
                    mask.resize(mask.size() + 4, 0); // multiples of 4 per spec
                }
                mask[byte] = static_cast<std::uint8_t>(mask[byte] | (1u << (bit % 8)));
            }
            return mask;
        }

        void process_acks(peer_id dst, conn &c, const utp_packet &pkt) {
            std::uint64_t acked_bytes           = 0;
            const std::uint64_t inflight_before = bytes_in_flight(c);
            bool acked_new                      = false;

            while (!c.inflight.empty() && seq_diff(c.inflight.front().seq, pkt.ack_nr) <= 0) {
                const inflight_pkt &p = c.inflight.front();
                acked_bytes += p.wire_bytes;
                if (p.transmissions == 1) {
                    update_rtt(c, cadmium::log::to_sim_double(state.now - p.sent_at));
                }
                acked_new = true;
                c.inflight.pop_front();
            }

            // Selective ACKs: count packets acked past still-missing ones.
            if (!pkt.sack_mask.empty() && !c.inflight.empty()) {
                apply_sack(dst, c, pkt, acked_bytes, acked_new);
            }

            if (acked_new) {
                c.dup_acks             = 0;
                c.consecutive_timeouts = 0;
                c.timeout = std::max(k_.min_timeout, c.rtt + 4.0 * c.rtt_var); // model-internal
                if (acked_bytes > 0 && inflight_before > 0) {
                    const double delay_sample =
                        static_cast<double>(pkt.timestamp_difference_microseconds) * 1e-6;
                    ledbat_update(c, static_cast<double>(acked_bytes),
                                  static_cast<double>(inflight_before), delay_sample);
                }
                if (c.state == conn_state::connected) {
                    packetize(dst, c);
                }
            } else if (pkt.ack_nr == c.last_ack_seen && !c.inflight.empty()) {
                if (++c.dup_acks >= k_.dup_ack_threshold) {
                    c.dup_acks = 0;
                    if (!c.inflight.empty()) {
                        cut_window(c);
                        retransmit(dst, c, c.inflight.front());
                    }
                }
            } else {
                // ack_nr changed without acking anything new (e.g. a
                // reordered/stale ack) — clear the counter so a later
                // duplicate of a *different* ack_nr value doesn't inherit
                // a count it never actually accrued.
                c.dup_acks = 0;
            }
            c.last_ack_seen = pkt.ack_nr;
        }

        void apply_sack(peer_id dst, conn &c, const utp_packet &pkt, std::uint64_t &acked_bytes,
                        bool &acked_new) {
            std::vector<std::uint16_t> to_erase;
            int acked_after = 0;
            // Walk inflight from newest to oldest counting sacked packets.
            for (auto it = c.inflight.rbegin(); it != c.inflight.rend(); ++it) {
                const std::int16_t off =
                    seq_diff(it->seq, static_cast<std::uint16_t>(pkt.ack_nr + 2));
                const bool sacked =
                    off >= 0 && static_cast<std::size_t>(off / 8) < pkt.sack_mask.size() &&
                    (pkt.sack_mask[static_cast<std::size_t>(off / 8)] >> (off % 8)) & 1u;
                if (sacked) {
                    acked_bytes += it->wire_bytes;
                    if (it->transmissions == 1) {
                        update_rtt(c, cadmium::log::to_sim_double(state.now - it->sent_at));
                    }
                    to_erase.push_back(it->seq);
                    acked_new = true;
                    ++acked_after;
                } else {
                    it->sacked_past += acked_after;
                }
            }
            for (const std::uint16_t seq : to_erase) {
                for (auto it = c.inflight.begin(); it != c.inflight.end(); ++it) {
                    if (it->seq == seq) {
                        c.inflight.erase(it);
                        break;
                    }
                }
            }
            // BEP 29: >= 3 packets acked past a hole marks it lost.
            std::vector<std::uint16_t> lost;
            for (const auto &p : c.inflight) {
                if (p.sacked_past >= k_.dup_ack_threshold) {
                    lost.push_back(p.seq);
                }
            }
            if (!lost.empty()) {
                cut_window(c);
                for (auto &p : c.inflight) {
                    if (p.sacked_past >= k_.dup_ack_threshold) {
                        p.sacked_past = 0;
                        retransmit(dst, c, p);
                    }
                }
            }
        }

        // packet_rtt: a measured sample duration converted from the
        // scheduling clock (state.now - sent_at, a TIME) into double via
        // cadmium::log::to_sim_double at the call site — see the class-level
        // doc comment on why rtt/rtt_var/timeout are double, not TIME.
        void update_rtt(conn &c, double packet_rtt) {
            const double delta = c.rtt - packet_rtt;
            c.rtt_var += (std::abs(delta) - c.rtt_var) / 4.0;
            c.rtt += (packet_rtt - c.rtt) / 8.0;
        }

        void ledbat_update(conn &c, double acked_bytes, double in_flight, double delay_sample) {
            const double base      = base_delay(c, delay_sample);
            const double our_delay = delay_sample - base;
            const double target    = k_.target_delay;

            if (c.slow_start) {
                if (our_delay >= target) {
                    c.ssthres    = c.cwnd / 2.0;
                    c.slow_start = false;
                } else {
                    c.cwnd += acked_bytes; // exponential growth
                    if (c.ssthres > 0.0 && c.cwnd > c.ssthres) {
                        c.slow_start = false;
                    }
                    clamp_cwnd(c);
                    return;
                }
            }
            const double window_factor = acked_bytes / in_flight;
            const double delay_factor  = (target - our_delay) / target;
            c.cwnd += k_.gain_bytes_per_rtt * delay_factor * window_factor;
            clamp_cwnd(c);
        }

        double base_delay(conn &c, double sample) {
            const auto bucket =
                static_cast<std::int64_t>(cadmium::log::to_sim_double(state.now) / 60.0);
            if (c.base_hist.empty() || c.base_hist.back().first != bucket) {
                c.base_hist.emplace_back(bucket, sample);
            } else if (sample < c.base_hist.back().second) {
                c.base_hist.back().second = sample;
            }
            const auto horizon = bucket - static_cast<std::int64_t>(k_.base_delay_window / 60.0);
            while (!c.base_hist.empty() && c.base_hist.front().first < horizon) {
                c.base_hist.pop_front();
            }
            double best = c.base_hist.front().second;
            for (const auto &[b, m] : c.base_hist) {
                best = m < best ? m : best;
            }
            return best;
        }

        void clamp_cwnd(conn &c) {
            const auto floor_b = static_cast<double>(k_.min_packet);
            c.cwnd             = c.cwnd < floor_b ? floor_b : c.cwnd;
        }

        void cut_window(conn &c) {
            // At most one multiplicative cut per outstanding window.
            if (c.cut_valid && seq_diff(c.cut_seq, c.inflight.front().seq) >= 0) {
                return;
            }
            c.cwnd *= k_.loss_multiplier;
            clamp_cwnd(c);
            if (c.slow_start) {
                c.ssthres    = c.cwnd;
                c.slow_start = false;
            }
            c.cut_seq   = static_cast<std::uint16_t>(c.seq_nr - 1);
            c.cut_valid = true;
        }

        void retransmit(peer_id dst, conn &c, inflight_pkt &p) {
            utp_packet pkt   = header_for(c, packet_type::st_data);
            pkt.seq_nr       = p.seq;
            pkt.payload_size = p.stream_bytes;
            ++p.transmissions;
            p.sent_at       = state.now;
            c.last_activity = state.now;
            ++state.retransmits;
            queue_frame(dst, pkt, p.completing);
        }

        void on_timeout(peer_id peer, conn &c) {
            ++state.timeouts;
            ++c.consecutive_timeouts;
            c.timeout       = c.timeout * 2.0; // RTO doubling (model-internal double)
            c.cwnd          = static_cast<double>(k_.min_packet);
            c.last_activity = state.now;
            if (c.state == conn_state::syn_sent) {
                // Re-send the SYN.
                utp_packet syn{};
                syn.type                   = packet_type::st_syn;
                syn.connection_id          = c.conn_id_recv;
                syn.timestamp_microseconds = now_micros();
                syn.wnd_size               = static_cast<std::uint32_t>(k_.recv_window);
                syn.seq_nr                 = 1;
                queue_frame(peer, syn);
                return;
            }
            if (!c.inflight.empty()) {
                retransmit(peer, c, c.inflight.front());
            }
        }

        peer_id self_{};
        utp_constants k_{};
    };

} // namespace bt_utp
