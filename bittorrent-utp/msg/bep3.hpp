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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
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

        /// <pstrlen><pstr><8 reserved bytes><info_hash><peer_id>. The 8
        /// reserved bytes are always zero in every handshake this project
        /// models (no extension flags in scope), so they're accounted for
        /// here as a constant rather than stored as a field — same
        /// modeling choice as proto: nothing this project does varies
        /// them, so there's nothing for a field to represent.
        [[nodiscard]] std::uint64_t wire_size() const {
            const std::uint64_t pstrlen            = proto == protocol_id::bittorrent_1_0 ? 19 : 0;
            constexpr std::uint64_t reserved_bytes = 8;
            return 1 + pstrlen + reserved_bytes + info_hash.size() + peer_id.size();
        }

        friend bool operator==(const handshake &, const handshake &) = default;
    };

    inline std::ostream &operator<<(std::ostream &os, const handshake &h) {
        const std::ios::fmtflags saved = os.flags();
        const char saved_fill          = os.fill();
        os << "HANDSHAKE info_hash:" << std::hex << std::setfill('0');
        for (auto b : h.info_hash) {
            os << std::setw(2) << static_cast<int>(b);
        }
        os << " peer_id:";
        for (auto b : h.peer_id) {
            os << std::setw(2) << static_cast<int>(b);
        }
        os.flags(saved);
        os.fill(saved_fill);
        return os;
    }

    struct keepalive {
        friend bool operator==(const keepalive &, const keepalive &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
            return bep3_length_prefix_bytes;
        }
    };
    struct choke {
        friend bool operator==(const choke &, const choke &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
            return bep3_length_prefix_bytes + bep3_id_bytes;
        }
    };
    struct unchoke {
        friend bool operator==(const unchoke &, const unchoke &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
            return bep3_length_prefix_bytes + bep3_id_bytes;
        }
    };
    struct interested {
        friend bool operator==(const interested &, const interested &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
            return bep3_length_prefix_bytes + bep3_id_bytes;
        }
    };
    struct not_interested {
        friend bool operator==(const not_interested &, const not_interested &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
            return bep3_length_prefix_bytes + bep3_id_bytes;
        }
    };

    struct have {
        std::uint32_t index{};

        friend bool operator==(const have &, const have &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
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

        [[nodiscard]] std::uint64_t wire_size() const {
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

        [[nodiscard]] std::uint64_t wire_size() const {
            return bep3_length_prefix_bytes + bep3_id_bytes + 12;
        }
    };

    using bep3_variant = std::variant<keepalive, choke, unchoke, interested, not_interested, have,
                                      bitfield, request, piece, cancel>;

    /// Publicly derives from the variant (C++23 permits std::visit/get/
    /// holds_alternative on a type derived from std::variant, P2162) rather
    /// than just aliasing it, so bep3_msg can be a utp_socket PAYLOAD: the
    /// socket calls payload.wire_size() as a *member*, matching every other
    /// PAYLOAD type this project uses (app_chunk), and std::get/
    /// holds_alternative/std::visit on a bep3_msg keep working exactly as
    /// they did when it was a bare variant alias.
    struct bep3_msg : bep3_variant {
        using bep3_variant::bep3_variant;

        friend bool operator==(const bep3_msg &, const bep3_msg &) = default;

        [[nodiscard]] std::uint64_t wire_size() const {
            return std::visit([](const auto &alt) { return alt.wire_size(); }, *this);
        }
    };

    [[nodiscard]] inline std::uint64_t wire_size(const bep3_msg &m) {
        return m.wire_size();
    }

    inline std::ostream &operator<<(std::ostream &os, const have &h) {
        return os << "HAVE idx:" << h.index;
    }
    inline std::ostream &operator<<(std::ostream &os, const bitfield &b) {
        const auto held =
            static_cast<std::size_t>(std::count(b.pieces.begin(), b.pieces.end(), true));
        return os << "BITFIELD held:" << held << "/" << b.pieces.size();
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
