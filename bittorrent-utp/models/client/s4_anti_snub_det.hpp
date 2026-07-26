// SPDX-License-Identifier: BSD-2-Clause
/**
 * s4as_top: the anti-snub scenario for bead e332, second half of the
 * "policy integration" work (see s4_two_client_det.hpp for the first
 * half). Reuses s4_top's exact coupling shape (two clients, two
 * unidirectional bottleneck_channel instances) unchanged -- what differs
 * is only the *initial state* each client is constructed with, not the
 * topology:
 *
 *  - both clients own a complementary half of the pieces from
 *    construction (clientA: [0, total/2), clientB: [total/2, total)),
 *    instead of one being a full seed and the other empty. This is what
 *    a plain seed/leech scenario can't give us: choking_policy only
 *    tracks the *serving* direction (are we uploading to a peer that
 *    isn't reciprocating), and a pure leech never has anything to serve,
 *    while a pure seed is never interested in anything -- there is no
 *    reciprocity relationship to stall in either direction. With both
 *    sides holding something the other wants, both directions carry real
 *    piece data and reciprocity becomes meaningful.
 *  - chan_ba (B's uploads to A) is constructed permanently slow, instead
 *    of both channels being fast. Concretely this stalls the very signal
 *    clientA's choking_policy is watching for from clientB (data
 *    arriving from B) without touching clientB's own view of clientA
 *    (chan_ab stays fast, so B never has anything to snub A over) --
 *    a deliberate, single-direction stall, not a bidirectional one.
 *  - neither choking_policy needs initial_complete=true: both start
 *    genuinely incomplete, so the default (false) applies to both.
 *
 * Expected behavior (verified in test_s4_anti_snub_det.cpp): clientA
 * unchokes clientB quickly (clientB is its only, trivially-top-ranked,
 * interested peer), but since chan_ba is far too slow for any piece to
 * arrive within choking_policy::snub_seconds (60s) of that unchoke,
 * clientA's own rechoke() marks clientB snubbed and issues a CHOKE for
 * it -- the observable downstream effect of `snubbed`, which is
 * otherwise pure internal state (see choking_policy.hpp's rechoke()).
 */
#pragma once

#include <cadmium/engine/devs_engine_helpers.hpp>
#include <cadmium/engine/devs_runner.hpp>
#include <cadmium/logger/cadmium_log.hpp>
#include <cadmium/modeling/coupling.hpp>
#include <cadmium/modeling/named.hpp>

#include "../utp/bottleneck_channel.hpp"
#include "bittorrent_client.hpp"
#include "choking_policy.hpp"
#include "event_filter_sink.hpp"
#include "peer_wire.hpp"
#include "piece_selector.hpp"
#include <cstdint>
#include <memory>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace bt_utp {

    inline constexpr peer_id s4as_client_a_id                = 1;
    inline constexpr peer_id s4as_client_b_id                = 2;
    inline constexpr std::uint32_t s4as_total_pieces         = 4;
    inline constexpr std::uint32_t s4as_sub_pieces_per_piece = 16;
    inline constexpr std::uint32_t s4as_sub_piece_bytes      = 16384; // 16 KiB block
    inline constexpr double s4as_rate_ab = 1'000'000.0; // A->B: fast, unstalled direction
    // B->A: deliberately far too slow for a single 16 KiB sub-piece
    // (16384 / 100 = 163.84s) to arrive within snub_seconds (60s) of
    // clientA unchoking clientB -- the whole point of this scenario.
    inline constexpr double s4as_rate_ba    = 100.0;
    inline constexpr double s4as_prop_delay = 0.05;

    // clientA owns the first half of the pieces, clientB the other half
    // -- complementary, not seed/leech, so each is genuinely interested
    // in the other.
    inline std::vector<bool> s4as_partial_bitfield(std::uint32_t total, std::uint32_t from,
                                                   std::uint32_t to) {
        std::vector<bool> bits(total, false);
        for (std::uint32_t i = from; i < to; ++i) {
            bits[i] = true;
        }
        return bits;
    }

    using s4as_frame_t   = typename bittorrent_client_defs::net_frame_t; // utp_frame<wire_msg>
    using s4as_chan_defs = bottleneck_channel_defs_t<s4as_frame_t>;

    template <typename TIME>
    struct s4as_client_a_socket : public utp_socket<TIME, wire_msg>,
                                  cadmium::named<"client_a.socket"> {
        s4as_client_a_socket() : utp_socket<TIME, wire_msg>(s4as_client_a_id, utp_constants{}) {}
    };
    template <typename TIME>
    struct s4as_client_a_wire : public peer_wire<TIME>, cadmium::named<"client_a.wire"> {
        s4as_client_a_wire()
            : peer_wire<TIME>(s4as_total_pieces, s4as_sub_pieces_per_piece, s4as_sub_piece_bytes,
                              s4as_partial_bitfield(s4as_total_pieces, 0, s4as_total_pieces / 2),
                              s4as_client_b_id) {}
    };
    template <typename TIME>
    struct s4as_client_a_choke : public choking_policy<TIME>, cadmium::named<"client_a.choke"> {
        s4as_client_a_choke() : choking_policy<TIME>(s4as_total_pieces, s4as_sub_piece_bytes) {}
    };
    template <typename TIME>
    struct s4as_client_a_selector : public piece_selector<TIME>,
                                    cadmium::named<"client_a.selector"> {
        s4as_client_a_selector()
            : piece_selector<TIME>(s4as_total_pieces, s4as_sub_pieces_per_piece) {}
    };
    template <typename TIME>
    struct s4as_client_a : public bittorrent_client<TIME, s4as_client_a_socket, s4as_client_a_wire,
                                                    s4as_client_a_choke, s4as_client_a_selector>,
                           cadmium::named<"client_a"> {};

    template <typename TIME>
    struct s4as_client_b_socket : public utp_socket<TIME, wire_msg>,
                                  cadmium::named<"client_b.socket"> {
        s4as_client_b_socket() : utp_socket<TIME, wire_msg>(s4as_client_b_id, utp_constants{}) {}
    };
    template <typename TIME>
    struct s4as_client_b_wire : public peer_wire<TIME>, cadmium::named<"client_b.wire"> {
        s4as_client_b_wire()
            : peer_wire<TIME>(s4as_total_pieces, s4as_sub_pieces_per_piece, s4as_sub_piece_bytes,
                              s4as_partial_bitfield(s4as_total_pieces, s4as_total_pieces / 2,
                                                    s4as_total_pieces)) {} // passive: acceptor
    };
    template <typename TIME>
    struct s4as_client_b_choke : public choking_policy<TIME>, cadmium::named<"client_b.choke"> {
        s4as_client_b_choke() : choking_policy<TIME>(s4as_total_pieces, s4as_sub_piece_bytes) {}
    };
    template <typename TIME>
    struct s4as_client_b_selector : public piece_selector<TIME>,
                                    cadmium::named<"client_b.selector"> {
        s4as_client_b_selector()
            : piece_selector<TIME>(s4as_total_pieces, s4as_sub_pieces_per_piece) {}
    };
    template <typename TIME>
    struct s4as_client_b : public bittorrent_client<TIME, s4as_client_b_socket, s4as_client_b_wire,
                                                    s4as_client_b_choke, s4as_client_b_selector>,
                           cadmium::named<"client_b"> {};

    template <typename TIME>
    struct s4as_chan_ab : public bottleneck_channel<TIME, s4as_frame_t>, cadmium::named<"chan_ab"> {
        s4as_chan_ab()
            : bottleneck_channel<TIME, s4as_frame_t>(TIME{s4as_prop_delay}, s4as_rate_ab) {}
    };
    template <typename TIME>
    struct s4as_chan_ba : public bottleneck_channel<TIME, s4as_frame_t>, cadmium::named<"chan_ba"> {
        s4as_chan_ba()
            : bottleneck_channel<TIME, s4as_frame_t>(TIME{s4as_prop_delay}, s4as_rate_ba) {}
    };

    using s4as_submodels =
        cadmium::modeling::models_tuple<s4as_chan_ab, s4as_chan_ba, s4as_client_a, s4as_client_b>;

    using s4as_ic =
        std::tuple<cadmium::modeling::IC<s4as_client_a, bittorrent_client_defs::net_out,
                                         s4as_chan_ab, typename s4as_chan_defs::in>,
                   cadmium::modeling::IC<s4as_chan_ab, typename s4as_chan_defs::out, s4as_client_b,
                                         bittorrent_client_defs::net_in>,
                   cadmium::modeling::IC<s4as_client_b, bittorrent_client_defs::net_out,
                                         s4as_chan_ba, typename s4as_chan_defs::in>,
                   cadmium::modeling::IC<s4as_chan_ba, typename s4as_chan_defs::out, s4as_client_a,
                                         bittorrent_client_defs::net_in>>;

    template <typename TIME>
    struct s4as_top
        : public cadmium::modeling::devs::coupling<TIME, std::tuple<>, std::tuple<>, s4as_submodels,
                                                   std::tuple<>, std::tuple<>, s4as_ic,
                                                   cadmium::engine::devs::first_imminent>,
          cadmium::named<"s4as_top"> {};

    struct s4as_det_result {
        double finish{};
        std::string ndjson_log{};
    };

    // Same rationale as s4_two_client_det.hpp's own allowlist: sim_messages_collect
    // already carries choking_policy's choke_cmd_out decisions attributed to
    // their own named<> model id.
    inline const std::vector<std::string> s4as_log_event_allowlist = {
        "sim_messages_collect", "run_global_time", "run_info", "sim_info_init", "coor_info_init"};

    // Same eternal-timer situation as s4_top: run_until()'s return value is
    // always t_max, never infinity. 100s comfortably covers clientA's
    // expected unchoke (~t=10) and snub-triggered choke (~t=70) of
    // clientB, without waiting for the (deliberately, hugely delayed)
    // first sub-piece to actually arrive over chan_ba.
    // include_state defaults to false (see s3_two_client_det.hpp's own
    // run_s3_det doc comment for why); the experiment harness passes true to
    // get socket cwnd data via sim_state.
    inline s4as_det_result run_s4as_det(double t_max = 100.0, bool include_state = false) {
        auto buffer    = std::make_shared<std::ostringstream>();
        auto raw_sink  = std::make_shared<spdlog::sinks::ostream_sink_mt>(*buffer);
        auto allowlist = s4as_log_event_allowlist;
        if (include_state) {
            allowlist.push_back("sim_state");
        }
        auto sink   = std::make_shared<event_filter_sink>(raw_sink, allowlist);
        auto logger = std::make_shared<spdlog::logger>("s4as_det", sink);
        logger->set_pattern("%v");
        logger->set_level(spdlog::level::debug);
        cadmium::log::set_logger(logger);

        cadmium::engine::devs::runner<double, s4as_top> runner(0.0);
        const double finish = runner.run_until(t_max);

        cadmium::log::flush();
        cadmium::log::reset_logger();

        return {finish, buffer->str()};
    }

} // namespace bt_utp
