// Exact musical fractions.
//
// A quantize grid is 1/16 or 1/12 or 1/24, and the moment those become doubles the
// question "does this note sit on the grid?" needs an epsilon. They do not become
// doubles here: a Rational is two integers, always normalised, and converting one
// to ticks goes through the same deterministic rounding tuplets use.
#pragma once

#include <compare>
#include <cstdint>

#include "engine/core/Time.h"

namespace adx::core {

/// A normalised fraction. The denominator is always positive and the fraction is
/// always in lowest terms, so equality is field equality and no comparison needs a
/// common denominator.
struct Rational {
    std::int64_t numerator{0};
    std::int64_t denominator{1};

    [[nodiscard]] static constexpr std::int64_t gcd(std::int64_t a, std::int64_t b) noexcept {
        a = a < 0 ? -a : a;
        b = b < 0 ? -b : b;
        while (b != 0) {
            const std::int64_t t = a % b;
            a = b;
            b = t;
        }
        return a;
    }

    [[nodiscard]] static constexpr Rational make(std::int64_t num, std::int64_t den) noexcept {
        if (den == 0) {
            return Rational{0, 1};
        }
        if (den < 0) {
            num = -num;
            den = -den;
        }
        const std::int64_t divisor = gcd(num, den);
        if (divisor > 1) {
            num /= divisor;
            den /= divisor;
        }
        return Rational{num, den};
    }

    [[nodiscard]] constexpr double toDouble() const noexcept {
        return static_cast<double>(numerator) / static_cast<double>(denominator);
    }

    [[nodiscard]] friend constexpr Rational operator+(Rational lhs, Rational rhs) noexcept {
        return make(lhs.numerator * rhs.denominator + rhs.numerator * lhs.denominator,
                    lhs.denominator * rhs.denominator);
    }
    [[nodiscard]] friend constexpr Rational operator-(Rational lhs, Rational rhs) noexcept {
        return make(lhs.numerator * rhs.denominator - rhs.numerator * lhs.denominator,
                    lhs.denominator * rhs.denominator);
    }
    [[nodiscard]] friend constexpr Rational operator*(Rational lhs, Rational rhs) noexcept {
        return make(lhs.numerator * rhs.numerator, lhs.denominator * rhs.denominator);
    }
    [[nodiscard]] friend constexpr Rational operator/(Rational lhs, Rational rhs) noexcept {
        return make(lhs.numerator * rhs.denominator, lhs.denominator * rhs.numerator);
    }

    /// Ordering without converting to double: cross-multiply. Denominators are
    /// positive after normalisation, so the inequality direction is preserved.
    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(Rational lhs,
                                                                    Rational rhs) noexcept {
        return lhs.numerator * rhs.denominator <=> rhs.numerator * lhs.denominator;
    }
    [[nodiscard]] friend constexpr bool operator==(Rational lhs, Rational rhs) noexcept {
        return lhs.numerator == rhs.numerator && lhs.denominator == rhs.denominator;
    }
};

/// A fraction of a whole note, in ticks. `1/16` is a sixteenth note; `1/12` is an
/// eighth-note triplet. Rounds through divideSpan, so a grid that does not divide
/// PPQ exactly still lands on the same tick every time.
[[nodiscard]] constexpr Ticks ticksOfWholeNote(Rational fraction) noexcept {
    return divideSpan(Ticks{kPpq * 4}, fraction.numerator, fraction.denominator);
}

} // namespace adx::core
