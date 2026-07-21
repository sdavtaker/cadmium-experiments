// SPDX-License-Identifier: BSD-2-Clause
/**
 * BEP 3 (BitTorrent protocol) message set: real header fields; the piece
 * message's payload is abstracted to a byte count (payload_size), same
 * structural-modeling convention as utp_packet.hpp — only protocol headers
 * are modeled bit-for-bit, bodies are sizes.
 *
 * The handshake predates the length-prefixed message stream (BEP 3: it is
 * sent once, before any <length prefix><id>... message), so it is its own
 * type, not a bep3_msg alternative. keepalive is a zero-length message (no
 * id byte) BEP 3 uses in place of any real message to hold the connection
 * open; every other alternative is one of the 9 real BEP 3 messages
 * (choke/unchoke/interested/not_interested/have/bitfield/request/piece/
 * cancel) — port (DHT) is out of scope here, tracked separately.
 */
#pragma once

#include <array>
#include <cstdint>
#include <ios>
#include <ostream>
#include <variant>
#include <vector>

namespace bt_utp {

    /// BEP 3 message framing: a 4-byte length prefix (covering the id byte
    /// and payload, but not itself), then a 1-byte id for every message
    /// except keepalive (length prefix 0, no id, no payload).
    inline constexpr std::uint64_t bep3_length_prefix_bytes = 4;
    inline constexpr std::uint64_t bep3_id_bytes            = 1;

    /// Only one protocol string is used in practice ("BitTorrent
    /// protocol", pstrlen 19) — a tag rather than a stored string, since
    /// the literal bytes never vary for any peer this project models.
    enum class protocol_id : std::uint8_t {
        bittorrent_1_0 = 0, // "BitTorrent protocol", pstrlen 19
    };

    struct handshake {
        protocol_id proto = protocol_id::bittorrent_1_0;
        std::array<std::uint8_t, 20> info_hash{};
        std::array<std::uint8_t, 20> peer_id{};

        /// <pstrlen><pstr><8 reserved bytes><info_hash><peer_id>, all
        /// fixed-length except pstr, whose length is a function of proto.
        [[nodiscard]] std::uint64_t wire_size() const {
            const std::uint64_t pstrlen = proto == protocol_id::bittorrent_1_0 ? 19 : 0;
            return 1 + pstrlen + 8 + info_hash.size() + peer_id.size();
        }

        friend bool operator==(const handshake &, const handshake &) = default;
    };

    inline std::ostream &operator<<(std::ostream &os, const handshake &h) {
        const std::ios::fmtflags saved = os.flags();
        os << "HANDSHAKE info_hash:" << std::hex;
        for (auto b : h.info_hash) {
            os << static_cast<int>(b);
        }
        os << " peer_id:";
        for (auto b : h.peer_id) {
            os << static_cast<int>(b);
        }
        os.flags(saved);
        return os;
    }

    struct keepalive {
        friend bool operator==(const keepalive &, const keepalive &) = default;

        [[nodiscard]] static std::uint64_t wire_size() {
            return bep3_length_prefix_bytes;
        }
    };
    struct choke {
        friend bool operator==(const choke &, const choke &) = default;

        [[nodiscard]] static std::uint64_t wire_size() {
            return bep3_length_prefix_bytes + bep3_id_bytes;
        }
    };
    struct unchoke {
        friend bool operator==(const unchoke &, const unchoke &) = default;

        [[nodiscard]] static std::uint64_t wire_size() {
            return bep3_length_prefix_bytes + bep3_id_bytes;
        }
    };
    struct interested {
        friend bool operator==(const interested &, const interested &) = default;

        [[nodiscard]] static std::uint64_t wire_size() {
            return bep3_length_prefix_bytes + bep3_id_bytes;
        }
    };
    struct not_interested {
        friend bool operator==(const not_interested &, const not_interested &) = default;

        [[nodiscard]] static std::uint64_t wire_size() {
            return bep3_length_prefix_bytes + bep3_id_bytes;
        }
    };

    struct have {
        std::uint32_t index{};

        friend bool operator==(const have &, const have &) = default;

        [[nodiscard]] static std::uint64_t wire_size() {
            return bep3_length_prefix_bytes + bep3_id_bytes + 4;
        }
    };

    /// One bit per piece, real bits (not size-abstracted) — piece
    /// possession is exactly what peer_wire's request-pipeline and
    /// interest-flag logic reads. size() is the swarm's total piece count.
    struct bitfield {
        std::vector<bool> pieces{};

        friend bool operator==(const bitfield &, const bitfield &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
            return bep3_length_prefix_bytes + bep3_id_bytes + (pieces.size() + 7) / 8;
        }
    };

    struct request {
        std::uint32_t index{};
        std::uint32_t begin{};
        std::uint32_t length{};

        friend bool operator==(const request &, const request &) = default;

        [[nodiscard]] static std::uint64_t wire_size() {
            return bep3_length_prefix_bytes + bep3_id_bytes + 12;
        }
    };

    struct piece {
        std::uint32_t index{};
        std::uint32_t begin{};
        std::uint32_t payload_size{}; // abstracted block bytes

        friend bool operator==(const piece &, const piece &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
            return bep3_length_prefix_bytes + bep3_id_bytes + 8 + payload_size;
        }
    };

    struct cancel {
        std::uint32_t index{};
        std::uint32_t begin{};
        std::uint32_t length{};

        friend bool operator==(const cancel &, const cancel &) = default;

        [[nodiscard]] static std::uint64_t wire_size() {
            return bep3_length_prefix_bytes + bep3_id_bytes + 12;
        }
    };

    using bep3_msg = std::variant<keepalive, choke, unchoke, interested, not_interested, have,
                                  bitfield, request, piece, cancel>;

    [[nodiscard]] inline std::uint64_t wire_size(const bep3_msg &m) {
        return std::visit([](const auto &alt) { return alt.wire_size(); }, m);
    }

    inline std::ostream &operator<<(std::ostream &os, const have &h) {
        return os << "HAVE idx:" << h.index;
    }
    inline std::ostream &operator<<(std::ostream &os, const bitfield &b) {
        os << "BITFIELD[" << b.pieces.size() << "]:";
        for (bool p : b.pieces) {
            os << (p ? '1' : '0');
        }
        return os;
    }
    inline std::ostream &operator<<(std::ostream &os, const request &r) {
        return os << "REQUEST idx:" << r.index << " begin:" << r.begin << " len:" << r.length;
    }
    inline std::ostream &operator<<(std::ostream &os, const piece &p) {
        return os << "PIECE idx:" << p.index << " begin:" << p.begin
                  << " payload:" << p.payload_size;
    }
    inline std::ostream &operator<<(std::ostream &os, const cancel &c) {
        return os << "CANCEL idx:" << c.index << " begin:" << c.begin << " len:" << c.length;
    }
    inline std::ostream &operator<<(std::ostream &os, const keepalive &) {
        return os << "KEEPALIVE";
    }
    inline std::ostream &operator<<(std::ostream &os, const choke &) {
        return os << "CHOKE";
    }
    inline std::ostream &operator<<(std::ostream &os, const unchoke &) {
        return os << "UNCHOKE";
    }
    inline std::ostream &operator<<(std::ostream &os, const interested &) {
        return os << "INTERESTED";
    }
    inline std::ostream &operator<<(std::ostream &os, const not_interested &) {
        return os << "NOT_INTERESTED";
    }

    inline std::ostream &operator<<(std::ostream &os, const bep3_msg &m) {
        std::visit([&os](const auto &alt) { os << alt; }, m);
        return os;
    }

} // namespace bt_utp
