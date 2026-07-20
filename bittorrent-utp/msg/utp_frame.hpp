// SPDX-License-Identifier: BSD-2-Clause
/**
 * Addressed uTP frame carrying application payload objects.
 *
 * Like net_frame, but ST_DATA packets additionally carry the application
 * messages whose final stream byte lies inside this packet ("completing").
 * Bodies are abstracted to sizes, so the payload objects ride the packet
 * that completes them and the receiver delivers each object once its byte
 * range is contiguous — the message-boundary-preserving byte stream is a
 * deliberate structural choice for this model. `completing` is modeling
 * metadata: it adds no wire bytes.
 */
#pragma once

#include "peer_id.hpp"
#include "utp_packet.hpp"
#include <cstdint>
#include <ostream>
#include <vector>

namespace bt_utp {

    template <typename PAYLOAD> struct utp_frame {
        peer_id src{};
        peer_id dst{};
        utp_packet pkt{};
        std::vector<PAYLOAD> completing{};

        [[nodiscard]] std::uint64_t wire_size() const {
            return pkt.wire_size();
        }

        friend bool operator==(const utp_frame &, const utp_frame &) = default;
    };

    template <typename PAYLOAD>
    inline std::ostream &operator<<(std::ostream &os, const utp_frame<PAYLOAD> &f) {
        os << f.src << "->" << f.dst << " [" << f.pkt << "]";
        if (!f.completing.empty()) {
            os << " completing:" << f.completing.size();
        }
        return os;
    }

} // namespace bt_utp
