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
#include "utp_socket.hpp"
#include <limits>
#include <stdexcept>
#include <tuple>

namespace bt_utp {

    template <typename TIME> class traffic_source {
      public:
        using send_req = typename utp_socket_defs_t<app_chunk>::send_req;

        struct defs {
            struct out : public cadmium::out_port<send_req> {};
        };

        traffic_source() = default;
        traffic_source(peer_id dst, app_chunk chunk) : dst_(dst), chunk_(chunk) {}

        using state_type = bool; // true while the chunk is still pending
        state_type state = true;

        using input_ports  = std::tuple<>;
        using output_ports = std::tuple<typename defs::out>;

        void internal_transition() {
            state = false;
        }
        void external_transition(TIME, typename cadmium::make_message_box<input_ports>::type) {
            throw std::logic_error("traffic_source has no inputs");
        }
        typename cadmium::make_message_box<output_ports>::type output() const {
            typename cadmium::make_message_box<output_ports>::type box;
            cadmium::get_message<typename defs::out>(box).emplace(send_req{dst_, chunk_});
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
