// SPDX-License-Identifier: BSD-2-Clause
/**
 * Synthetic application payload for the standalone uTP transport stage:
 * an identified byte count, nothing more (bodies are sizes by design).
 */
#pragma once

#include <cstdint>
#include <ostream>

namespace bt_utp {

    struct app_chunk {
        std::uint64_t id{};
        std::uint64_t bytes{};

        [[nodiscard]] std::uint64_t wire_size() const {
            return bytes;
        }

        friend bool operator==(const app_chunk &, const app_chunk &) = default;
    };

    inline std::ostream &operator<<(std::ostream &os, const app_chunk &c) {
        return os << "chunk#" << c.id << "(" << c.bytes << "B)";
    }

} // namespace bt_utp
