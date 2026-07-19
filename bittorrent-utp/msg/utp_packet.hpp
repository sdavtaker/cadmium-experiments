// SPDX-License-Identifier: BSD-2-Clause
/**
 * uTP packet message type (BEP 29).
 *
 * Real header fields per BEP 29 "header format" (20 bytes on the wire, v1),
 * plus the optional Selective ACK extension. The data payload is abstracted
 * to a byte count (payload_size) per the project decision that only protocol
 * headers are modeled structurally; bodies are sizes.
 *
 * Sources: wiki/sources/source-BEP29-utp-ledbat.md,
 *          wiki/concepts/concept-utp-devs-model.md
 */
#pragma once

#include <cstdint>
#include <ostream>
#include <vector>

namespace bt_utp {

    enum class packet_type : std::uint8_t {
        st_data  = 0,
        st_fin   = 1,
        st_state = 2,
        st_reset = 3,
        st_syn   = 4,
    };

    inline std::ostream &operator<<(std::ostream &os, packet_type t) {
        switch (t) {
        case packet_type::st_data:
            return os << "ST_DATA";
        case packet_type::st_fin:
            return os << "ST_FIN";
        case packet_type::st_state:
            return os << "ST_STATE";
        case packet_type::st_reset:
            return os << "ST_RESET";
        case packet_type::st_syn:
            return os << "ST_SYN";
        }
        return os << "ST_?";
    }

    /// BEP 29 v1 header is 20 bytes; a present SACK extension adds
    /// 2 bytes (extension type + len) plus the bitmask bytes.
    inline constexpr std::uint64_t utp_header_bytes = 20;

    struct utp_packet {
        packet_type type = packet_type::st_data;
        std::uint8_t ver = 1;
        std::uint16_t connection_id{};
        std::uint32_t timestamp_microseconds{};
        std::uint32_t timestamp_difference_microseconds{};
        std::uint32_t wnd_size{};
        std::uint16_t seq_nr{};
        std::uint16_t ack_nr{};
        /// Selective ACK bitmask (BEP 29 extension 1). Empty = extension
        /// absent. First bit acks seq ack_nr + 2; size must be a multiple
        /// of 4 bytes when present.
        std::vector<std::uint8_t> sack_mask{};
        /// Abstracted data payload: byte count only (0 for non-ST_DATA).
        std::uint64_t payload_size{};

        /// Bytes this packet occupies on the wire (header + extension +
        /// abstracted payload) — the quantity channels and cwnd account.
        [[nodiscard]] std::uint64_t wire_size() const {
            const std::uint64_t ext = sack_mask.empty() ? 0 : 2 + sack_mask.size();
            return utp_header_bytes + ext + payload_size;
        }

        friend bool operator==(const utp_packet &, const utp_packet &) = default;
    };

    inline std::ostream &operator<<(std::ostream &os, const utp_packet &p) {
        os << p.type << " conn:" << p.connection_id << " seq:" << p.seq_nr << " ack:" << p.ack_nr
           << " wnd:" << p.wnd_size << " ts:" << p.timestamp_microseconds
           << " tsdiff:" << p.timestamp_difference_microseconds;
        if (!p.sack_mask.empty()) {
            os << " sack[" << p.sack_mask.size() << "B]";
        }
        return os << " payload:" << p.payload_size << " wire:" << p.wire_size();
    }

} // namespace bt_utp
