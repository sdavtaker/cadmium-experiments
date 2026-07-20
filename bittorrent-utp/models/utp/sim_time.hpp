// SPDX-License-Identifier: BSD-2-Clause
/**
 * SimTime: the TIME concept this experiment line's transport models require
 * — exactly cadmium::concepts::Time (totally ordered, regular, +/-,
 * has_infinity). Deliberately not restricted to std::floating_point: DEVS
 * causality is only exact under exact-arithmetic time types (see
 * wiki/sources/source-VDW14-devs-time-datatype.md — floating-point virtual
 * time can silently reorder simultaneous events after enough accumulated
 * rounding error, invisibly, over long runs).
 *
 * double remains the practical default. cdcommons::time::decimal<Exp,Raw>
 * (this project's own established exact-time type for real experiments —
 * a fixed-point raw*10^Exp integer, so +/- are exact integer ops with no
 * denominator growth) is the supported exact-arithmetic reference.
 * RationalTime (../../rational_time.hpp) was tried first and rejected:
 * its naive long long numerator/denominator overflows within ~30-40
 * chained heterogeneous-denominator operations (confirmed with UBSan on a
 * probe mirroring an RTT-EWMA-shaped update), which a real transfer's
 * update count exceeds by a wide margin. decimal's fixed scale sidesteps
 * that failure mode entirely — see memory-vault-kdag and
 * wiki/concepts/concept-time-representation-des.md.
 *
 * decimal deliberately has no operator/ (dividing a fixed-point value
 * rarely lands on an exactly-representable result at the same scale, so
 * the type doesn't offer an operation whose result would silently need
 * rounding) and no static_cast-invokable integer constructor (only the
 * named factories from_scaled/from_whole). This is not a gap to work
 * around: the simulator/engine layer (state.now, last_activity,
 * time_advance(), scheduling comparisons) never needs to divide a TIME
 * value — only MODEL-internal arithmetic does (e.g. an RTT EWMA), and
 * that arithmetic isn't operating on the scheduling clock's type to begin
 * with. Model code should keep duration-like internal state (RTT
 * estimates, smoothed averages, measured samples) in its own natural
 * representation (double), exactly like any other model parameter, and
 * convert explicitly at the one boundary where such a value needs to
 * become — or came from — a scheduling-clock TIME: seconds_converter
 * (double -> TIME, e.g. scheduling an RTO deadline) and
 * cadmium::log::to_sim_double (TIME -> double, e.g. measuring an elapsed
 * duration to feed back into an EWMA) are that pair of conversions.
 */
#pragma once

#include <cadmium/concepts/common_concepts.hpp>
#include <cadmium/logger/cadmium_log.hpp>

#include <cdcommons/time/decimal.hpp>
#include <cmath>
#include <concepts>
#include <limits>

namespace bt_utp {

    template <typename T>
    concept SimTime = cadmium::concepts::Time<T>;

    /// Converts a config value expressed in seconds (as a human-written
    /// decimal, e.g. utp_constants::min_timeout, or a computed duration
    /// such as a byte-count/rate service time) into TIME. The primary
    /// template works for TIME=double (identity cast); every other TIME
    /// type used with this experiment line's models must provide an
    /// explicit specialization — there is no universally-correct way to
    /// turn an arbitrary double into an exact value, so this is a
    /// deliberate compile-time prompt rather than a silent, possibly-wrong
    /// conversion.
    template <SimTime TIME> struct seconds_converter {
        static TIME convert(double seconds) {
            return static_cast<TIME>(seconds);
        }
    };

    /// decimal<Exp,Raw>: round to the nearest raw unit at the type's own
    /// fixed resolution (e.g. Exp=-6 rounds to the nearest microsecond).
    /// This is a bounded, single-step rounding at the point of conversion —
    /// not the compounding, magnitude-dependent rounding error that makes
    /// floating-point virtual time unsafe (source-VDW14-devs-time-datatype.md)
    /// — the same deliberate, bounded-precision tradeoff a fixed clock tick
    /// makes in any discrete-event simulator.
    template <int Exp, std::signed_integral Raw>
    struct seconds_converter<cdcommons::time::decimal<Exp, Raw>> {
        using time_type = cdcommons::time::decimal<Exp, Raw>;
        static time_type convert(double seconds) {
            const double scale = std::pow(10.0, -Exp);
            return time_type::from_scaled(static_cast<Raw>(std::llround(seconds * scale)));
        }
    };

} // namespace bt_utp

namespace cadmium::log {
    /// Generic to_sim_double for any decimal<Exp,Raw> (the primary template
    /// in cadmium_log.hpp does static_cast<double>(t), which decimal has no
    /// conversion operator for). vdw14-devs/main.cpp defines a narrower,
    /// non-template version of this for one specific decimal<-3> instance;
    /// this overload covers every Exp/Raw combination this project uses.
    template <int Exp, std::signed_integral Raw>
    inline double to_sim_double(const cdcommons::time::decimal<Exp, Raw> &t) noexcept {
        using T = cdcommons::time::decimal<Exp, Raw>;
        if (t == std::numeric_limits<T>::infinity()) {
            return std::numeric_limits<double>::infinity();
        }
        if (t == std::numeric_limits<T>::neg_infinity()) {
            return -std::numeric_limits<double>::infinity();
        }
        return static_cast<double>(t.raw_value()) * std::pow(10.0, Exp);
    }
} // namespace cadmium::log
