// SPDX-License-Identifier: BSD-2-Clause
/**
 * Addressed network frame: the unit every channel/topology model carries.
 *
 * src/dst addressing exists from the two-client stage onward so the stage-6
 * ISP-switch topology can route on dst without a message refactor
 * (wiki/concepts/concept-utp-devs-model.md, structural decision 2).
 */
#pragma once

#include "peer_id.hpp"
#include "utp_packet.hpp"
#include <cstdint>
#include <ostream>

namespace bt_utp {

    struct net_frame {
        peer_id src{};
        peer_id dst{};
        utp_packet pkt{};

        [[nodiscard]] std::uint64_t wire_size() const {
            return pkt.wire_size();
        }

        friend bool operator==(const net_frame &, const net_frame &) = default;
    };

    inline std::ostream &operator<<(std::ostream &os, const net_frame &f) {
        return os << f.src << "->" << f.dst << " [" << f.pkt << "]";
    }

} // namespace bt_utp
