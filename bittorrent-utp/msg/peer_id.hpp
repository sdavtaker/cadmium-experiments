// SPDX-License-Identifier: BSD-2-Clause
/**
 * Shared peer identifier type, used by every addressed frame envelope
 * (net_frame, utp_frame). Centralized so translation units that include
 * more than one frame header don't see conflicting definitions.
 */
#pragma once

#include <cstdint>

namespace bt_utp {

    using peer_id = std::uint32_t;

} // namespace bt_utp
