// SPDX-License-Identifier: BSD-2-Clause
#pragma once

#include <cadmium/logger/cadmium_log.hpp>

#include <compare>
#include <limits>
#include <numeric>
#include <ostream>
#include <stdexcept>

// Exact rational time for Cadmium experiments.
// Self-contained (no Boost); uses std::gcd for reduction.
// Satisfies cadmium::concepts::Time: totally_ordered, regular,
// supports +/-, and provides std::numeric_limits::infinity().
class RationalTime {
    long long num_ = 0;
    long long den_ = 1;
    bool inf_      = false;

    struct inf_tag {};
    explicit RationalTime(inf_tag) noexcept : num_{1}, den_{0}, inf_{true} {}

    void reduce() noexcept {
        if (den_ < 0) {
            num_ = -num_;
            den_ = -den_;
        }
        long long g = std::gcd(std::abs(num_), den_);
        num_ /= g;
        den_ /= g;
    }

  public:
    RationalTime() noexcept = default;

    RationalTime(long long n, long long d) : num_{n}, den_{d} {
        if (d == 0)
            throw std::domain_error("RationalTime: denominator is zero");
        reduce();
    }

    explicit RationalTime(long long n) : num_{n}, den_{1} {}

    static RationalTime infinity() noexcept {
        return RationalTime{inf_tag{}};
    }
    bool is_inf() const noexcept {
        return inf_;
    }

    long long numerator() const noexcept {
        return num_;
    }
    long long denominator() const noexcept {
        return den_;
    }

    bool operator==(const RationalTime &o) const noexcept {
        if (inf_ && o.inf_)
            return true;
        if (inf_ || o.inf_)
            return false;
        return num_ == o.num_ && den_ == o.den_;
    }

    std::strong_ordering operator<=>(const RationalTime &o) const noexcept {
        if (inf_ && o.inf_)
            return std::strong_ordering::equal;
        if (inf_)
            return std::strong_ordering::greater;
        if (o.inf_)
            return std::strong_ordering::less;
        return (num_ * o.den_) <=> (o.num_ * den_);
    }

    RationalTime operator+(const RationalTime &o) const {
        if (inf_ || o.inf_)
            return infinity();
        return {num_ * o.den_ + o.num_ * den_, den_ * o.den_};
    }

    RationalTime operator-(const RationalTime &o) const {
        if (inf_)
            return infinity();
        return {num_ * o.den_ - o.num_ * den_, den_ * o.den_};
    }

    RationalTime operator/(const RationalTime &o) const {
        if (inf_)
            return infinity();
        return {num_ * o.den_, den_ * o.num_};
    }

    friend std::ostream &operator<<(std::ostream &os, const RationalTime &t) {
        if (t.inf_)
            return os << "inf";
        if (t.den_ == 1)
            return os << t.num_;
        return os << t.num_ << "/" << t.den_;
    }
};

template <> struct std::numeric_limits<RationalTime> {
    static constexpr bool is_specialized = true;
    static constexpr bool has_infinity   = true;
    static RationalTime infinity() noexcept {
        return RationalTime::infinity();
    }
};

namespace cadmium::log {
    template <> inline double to_sim_double(const RationalTime &t) noexcept {
        if (t.is_inf())
            return std::numeric_limits<double>::infinity();
        return static_cast<double>(t.numerator()) / static_cast<double>(t.denominator());
    }
} // namespace cadmium::log
