// Musical time.
//
// Musical time is integer ticks at PPQ = 3840. Float beats accumulate error under
// repeated edits, make `a + b == b + a` unreliable, and turn "is this note exactly
// on the grid?" into a tolerance question. Iteration one used `float startBeat` and
// paid for it in quantize and loop-wrap arithmetic (phase_2.md §4.1).
//
// Includes nothing that the realtime ban list forbids: Phase 3 renders from a
// projection of this model and needs these types below the audio callback.
#pragma once

#include <compare>
#include <cstdint>

namespace adx::core {

/// Ticks per quarter note.
///
/// 3840 = 2^8 x 3 x 5, so it divides exactly by every grid and tuplet a musician
/// actually uses - 2, 3, 4, 5, 6, 8, 10, 12, 15, 16, 20, 24, 32, 48, 64, 96, 128,
/// 192, 240, 320, 384, 480, 960, 1920 - including 128th-note quintuplets.
///
/// No PPQ divides by 7, so a 7-tuplet rounds. That is accepted and made
/// *deterministic* by divideSpan() below; reproducibility was the requirement, not
/// exactness (phase_2.md §4.1).
inline constexpr std::int64_t kPpq = 3840;

/// A musical position or duration, in ticks. Signed, because a delta is a Ticks.
struct Ticks {
    std::int64_t value{0};

    friend constexpr auto operator<=>(const Ticks&, const Ticks&) noexcept = default;

    constexpr Ticks& operator+=(Ticks other) noexcept {
        value += other.value;
        return *this;
    }
    constexpr Ticks& operator-=(Ticks other) noexcept {
        value -= other.value;
        return *this;
    }
    [[nodiscard]] friend constexpr Ticks operator+(Ticks lhs, Ticks rhs) noexcept {
        return Ticks{lhs.value + rhs.value};
    }
    [[nodiscard]] friend constexpr Ticks operator-(Ticks lhs, Ticks rhs) noexcept {
        return Ticks{lhs.value - rhs.value};
    }
    [[nodiscard]] friend constexpr Ticks operator-(Ticks operand) noexcept {
        return Ticks{-operand.value};
    }
    [[nodiscard]] friend constexpr Ticks operator*(Ticks lhs, std::int64_t rhs) noexcept {
        return Ticks{lhs.value * rhs};
    }
};

/// A position or duration in frames. Sample rate is never carried with it: the
/// conversion needs a TempoMap anyway, and that is where the rate is supplied.
struct Samples {
    std::int64_t value{0};

    friend constexpr auto operator<=>(const Samples&, const Samples&) noexcept = default;

    [[nodiscard]] friend constexpr Samples operator+(Samples lhs, Samples rhs) noexcept {
        return Samples{lhs.value + rhs.value};
    }
    [[nodiscard]] friend constexpr Samples operator-(Samples lhs, Samples rhs) noexcept {
        return Samples{lhs.value - rhs.value};
    }
};

/// Display and UI only, never storage. Anything that persists is Ticks.
using Beats = double;

/// A position decomposed against the meter map. All three fields are zero-based:
/// the first beat of the first bar is 0:0:0, which makes a duration and a position
/// the same arithmetic and is why a pattern eight bars long has LENGTH=8:0:0.
struct BarBeatTick {
    std::int64_t bar{0};
    std::int64_t beat{0};
    std::int64_t tick{0};

    friend constexpr auto operator<=>(const BarBeatTick&, const BarBeatTick&) noexcept = default;
};

/// Ticks per quarter note scaled to one beat of the given meter denominator.
///
/// A beat in 6/8 is an eighth note, so it is half a quarter note's ticks. Integer
/// division is exact for every denominator that divides 3840 - which is every
/// denominator the format accepts (Validate rejects the rest).
[[nodiscard]] constexpr std::int64_t ticksPerBeat(std::int64_t denominator) noexcept {
    return denominator > 0 ? (kPpq * 4) / denominator : kPpq;
}

/// The i-th of n equal divisions of `span`, rounded deterministically.
///
/// This is the whole of the tuplet rounding rule: `(span * i + n/2) / n`, integer
/// arithmetic, so the same pattern always compiles to the same ticks no matter how
/// many times it is recompiled. That reproducibility - not exactness - is what
/// hot-reload and offline-export agreement actually require (FINAL_PLAN §3.1).
[[nodiscard]] constexpr Ticks divideSpan(Ticks span, std::int64_t i, std::int64_t n) noexcept {
    if (n <= 0) {
        return Ticks{0};
    }
    const std::int64_t scaled = span.value * i;
    // Round half away from zero, so a negative span divides symmetrically.
    const std::int64_t bias = scaled >= 0 ? n / 2 : -(n / 2);
    return Ticks{(scaled + bias) / n};
}

} // namespace adx::core
