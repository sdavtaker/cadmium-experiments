// SPDX-License-Identifier: BSD-2-Clause
/**
 * bittorrent_client: the coupled model wiring one uTP socket, one peer_wire
 * FSM, and the two policy slots (choking policy, piece selector) into a
 * single client component — decision 0.3.5's "coupled shape from day one":
 * stage 3 plugs in the stub policies; stage 4 (bead e332) swaps only the
 * policy atomic types, unchanged wiring — SOCKET and WIRE are always
 * utp_socket/peer_wire respectively (never swapped), so their port types
 * are referenced directly via peer_wire_defs/utp_socket_defs_t<wire_msg>;
 * CHOKE_POLICY and SELECTOR are the genuinely swappable slots, so their
 * IC entries reference `typename CHOKE_POLICY<TIME>::defs::...` /
 * `typename SELECTOR<TIME>::defs::...` generically rather than hardcoding
 * stub_always_unchoke_defs/stub_sequential_selector_defs — any atomic
 * plugged in for either slot just needs its own `defs` member to expose
 * the same obs_in/choke_cmd_out (ChokingPolicy) or obs_in/plan_out
 * (PieceSelector) port *names*, matching stub_always_unchoke/
 * stub_sequential_selector's shape (already true of choking_policy/
 * piece_selector, the stage-4 real atomics).
 *
 * SOCKET/WIRE/CHOKE_POLICY/SELECTOR are template-template parameters, not
 * bare constructor calls, because none of the four submodels are
 * default-constructible (utp_socket needs a self peer_id; peer_wire needs
 * piece geometry, initial possession, and an optional connect-to peer;
 * the policy atomics need their own config) — cadmium's models_tuple
 * requires default-constructible `template<typename TIME> class`
 * submodels, so each caller supplies a thin per-instance wrapper that
 * bakes its own constructor arguments into a default constructor (the
 * same pattern test_msg_channel.cpp uses for bottleneck_channel). Each
 * wrapper must publicly inherit from exactly utp_socket<TIME, wire_msg>,
 * peer_wire<TIME>, a ChokingPolicy-shaped atomic, and a PieceSelector-
 * shaped atomic respectively.
 *
 * models_tuple order is SOCKET, WIRE, SELECTOR, CHOKE_POLICY — matching
 * decision 0.7's SELECT tie-break priority (channels -> sockets ->
 * peer_wire -> piece_selector -> choking_policy -> everything else):
 * cadmium::engine::devs::first_imminent picks the lowest tuple index among
 * simultaneously-imminent submodels, so this order *is* the priority.
 */
#pragma once

#include <cadmium/engine/devs_engine_helpers.hpp>
#include <cadmium/modeling/coupling.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../utp/utp_socket.hpp"
#include "peer_wire.hpp"
#include <tuple>

namespace bt_utp {

    struct bittorrent_client_defs {
        using net_frame_t = typename utp_socket_defs_t<wire_msg>::frame_t;

        struct net_in : public cadmium::in_port<net_frame_t> {};
        struct net_out : public cadmium::out_port<net_frame_t> {};
    };

    template <typename TIME, template <typename> class SOCKET, template <typename> class WIRE,
              template <typename> class CHOKE_POLICY, template <typename> class SELECTOR>
    using bittorrent_client = cadmium::modeling::devs::coupling<
        TIME, std::tuple<typename bittorrent_client_defs::net_in>,
        std::tuple<typename bittorrent_client_defs::net_out>,
        cadmium::modeling::models_tuple<SOCKET, WIRE, SELECTOR, CHOKE_POLICY>,
        std::tuple<cadmium::modeling::EIC<typename bittorrent_client_defs::net_in, SOCKET,
                                          typename utp_socket_defs_t<wire_msg>::net_in>>,
        std::tuple<cadmium::modeling::EOC<SOCKET, typename utp_socket_defs_t<wire_msg>::net_out,
                                          typename bittorrent_client_defs::net_out>>,
        std::tuple<
            cadmium::modeling::IC<SOCKET, typename utp_socket_defs_t<wire_msg>::app_deliver, WIRE,
                                  typename peer_wire_defs::wire_in>,
            cadmium::modeling::IC<WIRE, typename peer_wire_defs::wire_out, SOCKET,
                                  typename utp_socket_defs_t<wire_msg>::app_send>,
            cadmium::modeling::IC<WIRE, typename peer_wire_defs::obs_out, SELECTOR,
                                  typename SELECTOR<TIME>::defs::obs_in>,
            cadmium::modeling::IC<WIRE, typename peer_wire_defs::obs_out, CHOKE_POLICY,
                                  typename CHOKE_POLICY<TIME>::defs::obs_in>,
            cadmium::modeling::IC<SELECTOR, typename SELECTOR<TIME>::defs::plan_out, WIRE,
                                  typename peer_wire_defs::plan_in>,
            cadmium::modeling::IC<CHOKE_POLICY, typename CHOKE_POLICY<TIME>::defs::choke_cmd_out,
                                  WIRE, typename peer_wire_defs::choke_cmd_in>>,
        cadmium::engine::devs::first_imminent>;

} // namespace bt_utp
