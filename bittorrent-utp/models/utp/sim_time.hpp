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
 * probe mirroring this model's own RTT EWMA update), which a real
 * transfer's RTT/RTO update count exceeds by a wide margin. decimal's
 * fixed scale sidesteps that failure mode entirely — see
 * memory-vault-kdag and wiki/concepts/concept-time-representation-des.md.
 *
 * decimal has neither operator/ nor a static_cast-invokable integer
 * constructor (only the named factories from_scaled/from_whole), so this
 * experiment line's models must not assume either is available generically
 * — hence no division/construction requirement in the concept itself, and
 * the two customization points below (seconds_converter, divide_by) that
 * every TIME type must specialize instead of relying on operators that not
 * every exact TIME type has.
 */
#pragma once

#include <cadmium/concepts/common_concepts.hpp>
#include <cadmium/logger/cadmium_log.hpp>

#include <cdcommons/time/decimal.hpp>
#include <cmath>
#include <concepts>
#include <cstdint>
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

    /// Divides a TIME duration by a small positive integer constant (used
    /// for RTT EWMA weighting: rtt +/-= delta / 4, delta / 8). The default
    /// uses operator/, which double and RationalTime both have; decimal has
    /// no operator/ at all, so it gets its own overload operating directly
    /// on the raw scaled integer — exact truncating integer division, with
    /// no denominator or growth of any kind, unlike rational division.
    template <typename TIME> TIME divide_by(TIME t, long long n) {
        return t / static_cast<TIME>(n);
    }

    template <int Exp, std::signed_integral Raw>
    cdcommons::time::decimal<Exp, Raw> divide_by(cdcommons::time::decimal<Exp, Raw> t,
                                                 long long n) {
        return cdcommons::time::decimal<Exp, Raw>::from_scaled(
            static_cast<Raw>(t.raw_value() / static_cast<Raw>(n)));
    }

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
