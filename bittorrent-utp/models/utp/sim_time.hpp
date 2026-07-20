// SPDX-License-Identifier: BSD-2-Clause
/**
 * SimTime: the TIME concept this experiment line's transport models require
 * — cadmium::concepts::Time (totally ordered, regular, +/-, has_infinity)
 * plus division (needed for byte/rate duration math) and constructibility
 * from an integer byte count.
 *
 * Deliberately not restricted to std::floating_point: DEVS causality is
 * only exact under exact-arithmetic time types (see
 * wiki/sources/source-VDW14-devs-time-datatype.md — floating-point virtual
 * time can silently reorder simultaneous events after enough accumulated
 * rounding error, invisibly, over long runs). double remains supported as
 * the practical default; RationalTime (../../rational_time.hpp) is the
 * exact-arithmetic reference used to measure how far double's results
 * diverge from the correct ones, not a type to special-case away.
 */
#pragma once

#include <cadmium/concepts/common_concepts.hpp>

#include <concepts>
#include <cstdint>

namespace bt_utp {

    template <typename T>
    concept SimTime = cadmium::concepts::Time<T> && requires(T a, T b, std::uint64_t n) {
        { a / b } -> std::same_as<T>;
        { static_cast<T>(n) } -> std::same_as<T>;
    };

    /// Converts a config value expressed in seconds (as a human-written
    /// decimal, e.g. utp_constants::min_timeout) into TIME. The default
    /// works for TIME=double; every other TIME type used with this
    /// experiment line's models must provide an explicit specialization —
    /// there is no universally-correct way to turn an arbitrary double into
    /// an exact value, so this is a deliberate compile-time prompt rather
    /// than a silent, possibly-wrong conversion.
    template <SimTime TIME> struct seconds_converter {
        static TIME convert(double seconds) {
            return static_cast<TIME>(seconds);
        }
    };

} // namespace bt_utp
