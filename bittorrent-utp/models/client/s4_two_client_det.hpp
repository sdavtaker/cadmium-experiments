// SPDX-License-Identifier: BSD-2-Clause
/**
 * s4_top: the stage-4 deterministic two-client scenario — same seed/leech
 * topology as s3_top (models/client/s3_two_client_det.hpp), but with the
 * real choking_policy/piece_selector atomics (bittorrent_client.hpp's
 * CHOKE_POLICY/SELECTOR slots) instead of stage-3's stub policies. Stage-3's
 * own coupling structure is unchanged (bead e332's whole premise: "swap
 * stubs for real atomics, type change only" — bittorrent_client.hpp itself
 * needed one fix first, see its own doc comment, to make the IC entries
 * generic over whichever CHOKE_POLICY/SELECTOR type is plugged in).
 *
 * clientA is a full seed *from construction* — choking_policy needs to be
 * told this explicitly (initial_complete=true) since a client that never
 * downloads anything would otherwise never observe enough obs_completion
 * events to ever learn have_complete_file should be true on its own (a
 * real, previously-undiscovered gap found while building this scenario,
 * fixed in choking_policy.hpp itself, not worked around here).
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

    inline constexpr peer_id s4_client_a_id                = 1;
    inline constexpr peer_id s4_client_b_id                = 2;
    inline constexpr std::uint32_t s4_total_pieces         = 40;
    inline constexpr std::uint32_t s4_sub_pieces_per_piece = 16;
    inline constexpr std::uint32_t s4_sub_piece_bytes      = 16384; // 16 KiB block
    inline constexpr std::uint64_t s4_piece_bytes =
        std::uint64_t{s4_sub_pieces_per_piece} * s4_sub_piece_bytes; // 256 KiB
    inline constexpr double s4_rate_ab    = 1'000'000.0;             // 1 MB/s data direction
    inline constexpr double s4_rate_ba    = 100'000'000.0;           // fast ACK return path
    inline constexpr double s4_prop_delay = 0.05;

    using s4_frame_t   = typename bittorrent_client_defs::net_frame_t; // utp_frame<wire_msg>
    using s4_chan_defs = bottleneck_channel_defs_t<s4_frame_t>;

    template <typename TIME>
    struct s4_client_a_socket : public utp_socket<TIME, wire_msg>,
                                cadmium::named<"client_a.socket"> {
        s4_client_a_socket() : utp_socket<TIME, wire_msg>(s4_client_a_id, utp_constants{}) {}
    };
    template <typename TIME>
    struct s4_client_a_wire : public peer_wire<TIME>, cadmium::named<"client_a.wire"> {
        s4_client_a_wire()
            : peer_wire<TIME>(s4_total_pieces, s4_sub_pieces_per_piece, s4_sub_piece_bytes,
                              std::vector<bool>(s4_total_pieces, true), s4_client_b_id) {}
    };
    template <typename TIME>
    struct s4_client_a_choke : public choking_policy<TIME>, cadmium::named<"client_a.choke"> {
        s4_client_a_choke()
            : choking_policy<TIME>(s4_total_pieces, s4_sub_piece_bytes, /*initial_complete=*/true) {
        }
    };
    template <typename TIME>
    struct s4_client_a_selector : public piece_selector<TIME>, cadmium::named<"client_a.selector"> {
        s4_client_a_selector() : piece_selector<TIME>(s4_total_pieces, s4_sub_pieces_per_piece) {}
    };
    template <typename TIME>
    struct s4_client_a : public bittorrent_client<TIME, s4_client_a_socket, s4_client_a_wire,
                                                  s4_client_a_choke, s4_client_a_selector>,
                         cadmium::named<"client_a"> {};

    template <typename TIME>
    struct s4_client_b_socket : public utp_socket<TIME, wire_msg>,
                                cadmium::named<"client_b.socket"> {
        s4_client_b_socket() : utp_socket<TIME, wire_msg>(s4_client_b_id, utp_constants{}) {}
    };
    template <typename TIME>
    struct s4_client_b_wire : public peer_wire<TIME>, cadmium::named<"client_b.wire"> {
        s4_client_b_wire()
            : peer_wire<TIME>(s4_total_pieces, s4_sub_pieces_per_piece, s4_sub_piece_bytes,
                              std::vector<bool>(s4_total_pieces, false)) {} // passive: acceptor
    };
    template <typename TIME>
    struct s4_client_b_choke : public choking_policy<TIME>, cadmium::named<"client_b.choke"> {
        s4_client_b_choke()
            : choking_policy<TIME>(s4_total_pieces, s4_sub_piece_bytes) {
        } // initial_complete defaults false: client B starts empty
    };
    template <typename TIME>
    struct s4_client_b_selector : public piece_selector<TIME>, cadmium::named<"client_b.selector"> {
        s4_client_b_selector() : piece_selector<TIME>(s4_total_pieces, s4_sub_pieces_per_piece) {}
    };
    template <typename TIME>
    struct s4_client_b : public bittorrent_client<TIME, s4_client_b_socket, s4_client_b_wire,
                                                  s4_client_b_choke, s4_client_b_selector>,
                         cadmium::named<"client_b"> {};

    template <typename TIME>
    struct s4_chan_ab : public bottleneck_channel<TIME, s4_frame_t>, cadmium::named<"chan_ab"> {
        s4_chan_ab() : bottleneck_channel<TIME, s4_frame_t>(TIME{s4_prop_delay}, s4_rate_ab) {}
    };
    template <typename TIME>
    struct s4_chan_ba : public bottleneck_channel<TIME, s4_frame_t>, cadmium::named<"chan_ba"> {
        s4_chan_ba() : bottleneck_channel<TIME, s4_frame_t>(TIME{s4_prop_delay}, s4_rate_ba) {}
    };

    using s4_submodels =
        cadmium::modeling::models_tuple<s4_chan_ab, s4_chan_ba, s4_client_a, s4_client_b>;

    using s4_ic = std::tuple<cadmium::modeling::IC<s4_client_a, bittorrent_client_defs::net_out,
                                                   s4_chan_ab, typename s4_chan_defs::in>,
                             cadmium::modeling::IC<s4_chan_ab, typename s4_chan_defs::out,
                                                   s4_client_b, bittorrent_client_defs::net_in>,
                             cadmium::modeling::IC<s4_client_b, bittorrent_client_defs::net_out,
                                                   s4_chan_ba, typename s4_chan_defs::in>,
                             cadmium::modeling::IC<s4_chan_ba, typename s4_chan_defs::out,
                                                   s4_client_a, bittorrent_client_defs::net_in>>;

    template <typename TIME>
    struct s4_top
        : public cadmium::modeling::devs::coupling<TIME, std::tuple<>, std::tuple<>, s4_submodels,
                                                   std::tuple<>, std::tuple<>, s4_ic,
                                                   cadmium::engine::devs::first_imminent>,
          cadmium::named<"s4_top"> {};

    struct s4_det_result {
        double finish{};
        std::string ndjson_log{};
    };

    // Same allowlist as s3_two_client_det.hpp: sim_messages_collect already
    // carries every model's decisions (including choking_policy/
    // piece_selector's own choke_cmd_out/plan_out messages) attributed to
    // their own named<> model id, so no separate sim_state event is needed
    // to satisfy "choking/selection decisions as their own model events".
    inline const std::vector<std::string> s4_log_event_allowlist = {
        "sim_messages_collect", "run_global_time", "run_info", "sim_info_init", "coor_info_init"};

    // Unlike s3_top, this system never fully passivates: choking_policy's
    // rechoke/optimistic timers reschedule themselves every 10s/30s
    // forever, regardless of whether the transfer is done, so
    // runner.run_until()'s return value here is *always* t_max, never
    // infinity -- that's expected (a real running client's choke
    // algorithm keeps ticking as long as the client is alive), not a
    // hang. The measured transfer completion (all 40 pieces delivered)
    // happens around t=51s; 100s leaves comfortable margin without
    // wastefully simulating hundreds of extra idle timer ticks past that
    // (t_max=200, s3_two_client_det.hpp's own default, cost ~150s of
    // simulated idle ticking with nothing left to do -- measured to take
    // several minutes of wall-clock on this Pi for no benefit).
    /// include_state defaults to false (see s3_two_client_det.hpp's own
    /// run_s3_det doc comment for why); the experiment harness passes true
    /// to get socket cwnd data via sim_state.
    inline s4_det_result run_s4_det(double t_max = 100.0, bool include_state = false) {
        auto buffer    = std::make_shared<std::ostringstream>();
        auto raw_sink  = std::make_shared<spdlog::sinks::ostream_sink_mt>(*buffer);
        auto allowlist = s4_log_event_allowlist;
        if (include_state) {
            allowlist.push_back("sim_state");
        }
        auto sink   = std::make_shared<event_filter_sink>(raw_sink, allowlist);
        auto logger = std::make_shared<spdlog::logger>("s4_det", sink);
        logger->set_pattern("%v");
        logger->set_level(spdlog::level::debug);
        cadmium::log::set_logger(logger);

        cadmium::engine::devs::runner<double, s4_top> runner(0.0);
        const double finish = runner.run_until(t_max);

        cadmium::log::flush();
        cadmium::log::reset_logger();

        return {finish, buffer->str()};
    }

} // namespace bt_utp
