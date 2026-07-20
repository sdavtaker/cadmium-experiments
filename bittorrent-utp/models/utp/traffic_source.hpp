// SPDX-License-Identifier: BSD-2-Clause
/**
 * One-shot traffic source for standalone uTP transport validation: emits a
 * single application-level chunk to a socket's app_send port at t=0, then
 * goes passive. The socket's own packetization/pipelining splits this into
 * as many wire packets as needed — one send here models one "file".
 */
#pragma once

#include <cadmium/modeling/message_box.hpp>
#include <cadmium/modeling/ports.hpp>

#include "../../msg/app_chunk.hpp"
#include "../../msg/peer_id.hpp"
#include "sim_time.hpp"
#include "utp_socket.hpp"
#include <limits>
#include <tuple>

namespace bt_utp {

    template <SimTime TIME> class traffic_source {
      public:
        using send_req = typename utp_socket_defs_t<app_chunk>::send_req;

        struct defs {
            struct out : public cadmium::out_port<send_req> {};
        };

        // Default-constructed is passive (state=false): only the
        // dst+chunk constructor arms the source, so a bare
        // default-constructed instance never emits spurious traffic.
        traffic_source() = default;
        traffic_source(peer_id dst, app_chunk chunk) : state(true), dst_(dst), chunk_(chunk) {}

        using state_type = bool; // true while the chunk is still pending
        state_type state = false;

        using input_ports  = std::tuple<>;
        using output_ports = std::tuple<typename defs::out>;

        void internal_transition() {
            state = false;
        }
        void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type) {
            // No input ports: nothing to do even if a scheduler calls this
            // defensively with an empty box.
        }
        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            if (state) {
                cadmium::get_message<typename defs::out>(box).emplace(send_req{dst_, chunk_});
            }
            return box;
        }
        TIME time_advance() const {
            return state ? TIME{} : std::numeric_limits<TIME>::infinity();
        }

      private:
        peer_id dst_{};
        app_chunk chunk_{};
    };

} // namespace bt_utp
