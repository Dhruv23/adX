// adx-thread: realtime
//
// Tap tempo and tempo nudge (phase_11.md §4.8).
//
// Tap tempo averages the intervals between the last few taps, rejecting one that is far
// from the others - a late or doubled tap must not move the tempo more than a couple of
// percent (tap_tempo_outliers). A gap of more than two seconds starts a new count.
//
// Nudge scales the master source's rate while it is held and returns it exactly when let
// go: the rate is computed from the base each time, never multiplied into itself, so a
// hundred nudges leave no drift (tempo_nudge_rate).
#pragma once

#include <array>
#include <cstdint>

namespace adx::session {

class TapTempo {
public:
    /// A tap at `seconds` (any monotonic clock). Returns the tempo the taps so far say,
    /// or 0 until there are two.
    double tap(double seconds) noexcept;
    void reset() noexcept {
        m_count = 0;
    }

private:
    static constexpr std::size_t kTaps = 5; // four intervals
    std::array<double, kTaps> m_times{};
    std::size_t m_count{0};
};

/// The rate a nudge gives: `base` sped up (direction > 0) or slowed (< 0) by `amount`
/// (0.04 is four per cent), or `base` itself when direction is 0.
[[nodiscard]] constexpr double nudgedRate(double base, int direction,
                                          double amount = 0.04) noexcept {
    if (direction > 0) {
        return base * (1.0 + amount);
    }
    if (direction < 0) {
        return base * (1.0 - amount);
    }
    return base;
}

} // namespace adx::session
