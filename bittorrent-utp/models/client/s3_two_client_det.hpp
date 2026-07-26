// SPDX-License-Identifier: BSD-2-Clause
/**
 * s3_top: the stage-3 deterministic two-client scenario — clientA (a seed
 * with a full bitfield) and clientB (an empty leech) connected through two
 * real bottleneck_channel instances (A->B data, B->A acks), each client
 * being a real bittorrent_client coupling (socket + peer_wire + stub
 * policies). A closed system: no external ports.
 *
 * bittorrent_client exposes only net_in/net_out (wire-level) ports — there
 * is no application-level "download complete" port to introspect from
 * outside, and cadmium's coordinator keeps its subcoordinators private, so
 * this project's established observability mechanism (decision 0.9: every
 * atomic's state_type has operator<<, NDJSON is the record of every
 * sim_state/sim_messages_collect event) is not just a nice-to-have here —
 * it is the *only* way to verify what happened inside the run. run_s3_det()
 * therefore installs a captured-string spdlog sink before running and
 * returns the full NDJSON trace alongside the finish time, so both the
 * golden-value ctest and the scripted trace audit read the same record.
 *
 * That sink is filtered (event_filter_sink.hpp) down to
 * s3_log_event_allowlist: measured on a full run, per-tick
 * coordinator/simulator bookkeeping events (sim_state, sim_info_advance,
 * coor_info_advance, coor_routing_*, sim_info_collect) outnumber
 * sim_messages_collect — the event that actually carries every BEP3 wire
 * exchange — roughly 15:1, and are why the unfiltered trace was 388MB
 * instead of the 54MB this filtering brings it to (with cadmium::named<>
 * applied; 826MB before that). None of the planned golden-value assertions
 * (completion time, REQUEST/PIECE count symmetry, distinct HAVE count) need
 * per-atomic state snapshots, only message traffic — a future test that
 * does can extend the allowlist with "sim_state".
 */
#pragma once

#include <cadmium/engine/devs_engine_helpers.hpp>
#include <cadmium/engine/devs_runner.hpp>
#include <cadmium/logger/cadmium_log.hpp>
#include <cadmium/modeling/coupling.hpp>
#include <cadmium/modeling/named.hpp>

#include "../utp/bottleneck_channel.hpp"
#include "bittorrent_client.hpp"
#include "event_filter_sink.hpp"
#include "peer_wire.hpp"
#include "stub_always_unchoke.hpp"
#include "stub_sequential_selector.hpp"
#include <cstdint>
#include <memory>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace bt_utp {

    // Swarm geometry for this scenario (decision: TIME = double throughout
    // stage 3+, per 0.1 — the exact-arithmetic cross-check that justified a
    // generic TIME was a stage-2-specific causality concern, already
    // resolved there).
    inline constexpr peer_id s3_client_a_id                = 1;
    inline constexpr peer_id s3_client_b_id                = 2;
    inline constexpr std::uint32_t s3_total_pieces         = 40;
    inline constexpr std::uint32_t s3_sub_pieces_per_piece = 16;
    inline constexpr std::uint32_t s3_sub_piece_bytes      = 16384; // 16 KiB block
    inline constexpr std::uint64_t s3_piece_bytes =
        std::uint64_t{s3_sub_pieces_per_piece} * s3_sub_piece_bytes; // 256 KiB
    inline constexpr double s3_rate_ab    = 1'000'000.0;             // 1 MB/s data direction
    inline constexpr double s3_rate_ba    = 100'000'000.0;           // fast ACK return path
    inline constexpr double s3_prop_delay = 0.05;

    using s3_frame_t   = typename bittorrent_client_defs::net_frame_t; // utp_frame<wire_msg>
    using s3_chan_defs = bottleneck_channel_defs_t<s3_frame_t>;

    template <typename TIME>
    struct s3_client_a_socket : public utp_socket<TIME, wire_msg>,
                                cadmium::named<"client_a.socket"> {
        s3_client_a_socket() : utp_socket<TIME, wire_msg>(s3_client_a_id, utp_constants{}) {}
    };
    template <typename TIME>
    struct s3_client_a_wire : public peer_wire<TIME>, cadmium::named<"client_a.wire"> {
        s3_client_a_wire()
            : peer_wire<TIME>(s3_total_pieces, s3_sub_pieces_per_piece, s3_sub_piece_bytes,
                              std::vector<bool>(s3_total_pieces, true), s3_client_b_id) {}
    };
    template <typename TIME>
    struct s3_client_a_selector : public stub_sequential_selector<TIME>,
                                  cadmium::named<"client_a.selector"> {
        s3_client_a_selector() : stub_sequential_selector<TIME>(s3_sub_pieces_per_piece) {}
    };
    template <typename TIME>
    struct s3_client_a : public bittorrent_client<TIME, s3_client_a_socket, s3_client_a_wire,
                                                  stub_always_unchoke, s3_client_a_selector>,
                         cadmium::named<"client_a"> {};

    template <typename TIME>
    struct s3_client_b_socket : public utp_socket<TIME, wire_msg>,
                                cadmium::named<"client_b.socket"> {
        s3_client_b_socket() : utp_socket<TIME, wire_msg>(s3_client_b_id, utp_constants{}) {}
    };
    template <typename TIME>
    struct s3_client_b_wire : public peer_wire<TIME>, cadmium::named<"client_b.wire"> {
        s3_client_b_wire()
            : peer_wire<TIME>(s3_total_pieces, s3_sub_pieces_per_piece, s3_sub_piece_bytes,
                              std::vector<bool>(s3_total_pieces, false)) {} // passive: acceptor
    };
    template <typename TIME>
    struct s3_client_b_selector : public stub_sequential_selector<TIME>,
                                  cadmium::named<"client_b.selector"> {
        s3_client_b_selector() : stub_sequential_selector<TIME>(s3_sub_pieces_per_piece) {}
    };
    template <typename TIME>
    struct s3_client_b : public bittorrent_client<TIME, s3_client_b_socket, s3_client_b_wire,
                                                  stub_always_unchoke, s3_client_b_selector>,
                         cadmium::named<"client_b"> {};

    template <typename TIME>
    struct s3_chan_ab : public bottleneck_channel<TIME, s3_frame_t>, cadmium::named<"chan_ab"> {
        s3_chan_ab() : bottleneck_channel<TIME, s3_frame_t>(TIME{s3_prop_delay}, s3_rate_ab) {}
    };
    template <typename TIME>
    struct s3_chan_ba : public bottleneck_channel<TIME, s3_frame_t>, cadmium::named<"chan_ba"> {
        s3_chan_ba() : bottleneck_channel<TIME, s3_frame_t>(TIME{s3_prop_delay}, s3_rate_ba) {}
    };

    using s3_submodels =
        cadmium::modeling::models_tuple<s3_chan_ab, s3_chan_ba, s3_client_a, s3_client_b>;

    using s3_ic = std::tuple<cadmium::modeling::IC<s3_client_a, bittorrent_client_defs::net_out,
                                                   s3_chan_ab, typename s3_chan_defs::in>,
                             cadmium::modeling::IC<s3_chan_ab, typename s3_chan_defs::out,
                                                   s3_client_b, bittorrent_client_defs::net_in>,
                             cadmium::modeling::IC<s3_client_b, bittorrent_client_defs::net_out,
                                                   s3_chan_ba, typename s3_chan_defs::in>,
                             cadmium::modeling::IC<s3_chan_ba, typename s3_chan_defs::out,
                                                   s3_client_a, bittorrent_client_defs::net_in>>;

    template <typename TIME>
    struct s3_top
        : public cadmium::modeling::devs::coupling<TIME, std::tuple<>, std::tuple<>, s3_submodels,
                                                   std::tuple<>, std::tuple<>, s3_ic,
                                                   cadmium::engine::devs::first_imminent>,
          cadmium::named<"s3_top"> {};

    struct s3_det_result {
        double finish{};
        std::string ndjson_log{};
    };

    // See the file-level comment for why sim_state and the coordinator/
    // simulator bookkeeping events are excluded.
    inline const std::vector<std::string> s3_log_event_allowlist = {
        "sim_messages_collect", "run_global_time", "run_info", "sim_info_init", "coor_info_init"};

    /// Runs the stage-3 deterministic scenario to completion (or t_max,
    /// whichever comes first), capturing the NDJSON trace (filtered to
    /// s3_log_event_allowlist, plus "sim_state" when include_state is set)
    /// into a string rather than stdout — this project's only way to verify
    /// what happened inside a run that exposes no application-level port.
    /// include_state defaults to false: per the file-level comment, sim_state
    /// outnumbers sim_messages_collect ~15:1, so it stays opt-in for callers
    /// (e.g. the experiment harness reading socket cwnd) rather than paid by
    /// every ctest invocation.
    inline s3_det_result run_s3_det(double t_max = 200.0, bool include_state = false) {
        auto buffer    = std::make_shared<std::ostringstream>();
        auto raw_sink  = std::make_shared<spdlog::sinks::ostream_sink_mt>(*buffer);
        auto allowlist = s3_log_event_allowlist;
        if (include_state) {
            allowlist.push_back("sim_state");
        }
        auto sink   = std::make_shared<event_filter_sink>(raw_sink, allowlist);
        auto logger = std::make_shared<spdlog::logger>("s3_det", sink);
        logger->set_pattern("%v");
        logger->set_level(spdlog::level::debug);
        cadmium::log::set_logger(logger);

        cadmium::engine::devs::runner<double, s3_top> runner(0.0);
        const double finish = runner.run_until(t_max);

        cadmium::log::flush();
        cadmium::log::reset_logger();

        return {finish, buffer->str()};
    }

} // namespace bt_utp
