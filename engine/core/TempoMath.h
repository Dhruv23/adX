// The tick <-> seconds <-> samples arithmetic, over borrowed arrays.
//
// Split out of TempoMap so that the main thread and the audio thread run *the same
// code* to place a note. TempoMap owns std::vectors and rebuilds its prefix sums on
// edit, which is right for the model and banned below the callback; a TempoView is
// two spans over data somebody else owns - the TempoMap on the main thread, the
// render snapshot's arena on the audio thread - and every conversion either side
// makes goes through the functions declared here.
//
// That is not tidiness. Phase 3's offline render has to match its realtime capture
// bit for bit (phase_3.md §4.10), and a second, "equivalent" implementation of
// toSamples on the audio side is exactly the kind of thing that agrees to within a
// sample on every test anyone thought to write and then disagrees on a tempo ramp.
//
// Includes nothing the realtime ban list forbids.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/core/Time.h"

namespace adx::core {

/// A tempo change. `ramp` means the tempo travels linearly in the tick domain from
/// here to the next event's bpm; without a next event a ramp is held constant,
/// because there is nothing to ramp toward.
struct TempoEvent {
    Ticks at;
    double bpm{120.0};
    bool ramp{false};

    [[nodiscard]] friend bool operator==(const TempoEvent&, const TempoEvent&) noexcept = default;
};

struct MeterEvent {
    Ticks at;
    std::uint16_t numerator{4};
    std::uint16_t denominator{4};

    [[nodiscard]] friend bool operator==(const MeterEvent&, const MeterEvent&) noexcept = default;
};

/// Tempo bounds. Not taste - arithmetic: below kMinBpm a segment's length overflows
/// anything useful, and above kMaxBpm a tick is shorter than a sample at 48 kHz, at
/// which point toTicks(toSamples(t)) stops being an identity.
inline constexpr double kMinBpm = 1.0;
inline constexpr double kMaxBpm = 999.0;

/// The highest tempo at which tick-to-sample conversion still round-trips exactly,
/// for a given rate: one tick has to be at least one sample long.
[[nodiscard]] constexpr double maxExactBpm(std::uint32_t sampleRate) noexcept {
    return (static_cast<double>(sampleRate) * 60.0) / static_cast<double>(kPpq);
}

/// A read-only tempo map: the events and the seconds elapsed at the start of each.
///
/// `cumSeconds[i]` is the wall-clock time at `events[i].at`. Both spans have the same
/// length and at least one element, at tick 0 - which is what TempoMap guarantees
/// and what every function below assumes rather than re-checks.
///
/// Every member is noexcept and allocation-free: binary search over `events`, then
/// closed-form arithmetic.
struct TempoView {
    std::span<const TempoEvent> events;
    std::span<const double> cumSeconds;

    [[nodiscard]] bool valid() const noexcept {
        return !events.empty() && events.size() == cumSeconds.size();
    }

    /// The index of the segment containing `at`.
    [[nodiscard]] std::size_t segmentAt(Ticks at) const noexcept;

    [[nodiscard]] double bpmAt(Ticks at) const noexcept;

    /// Wall-clock seconds elapsed from tick 0 to `at`. Negative ticks extrapolate
    /// backwards through the first segment's tempo, which is what a count-in needs.
    [[nodiscard]] double secondsAt(Ticks at) const noexcept;
    [[nodiscard]] Ticks ticksAtSeconds(double seconds) const noexcept;

    /// Rounds to the nearest frame, once, at the end - so where a note lands is a
    /// function of its tick and the rate and nothing else.
    [[nodiscard]] Samples toSamples(Ticks at, std::uint32_t sampleRate) const noexcept;
    [[nodiscard]] Ticks toTicks(Samples at, std::uint32_t sampleRate) const noexcept;

    /// The tick of the first tempo event strictly after `at`, or `fallback` when there
    /// is none. The scheduler splits a block here.
    [[nodiscard]] Ticks nextChangeAfter(Ticks at, Ticks fallback) const noexcept;
};

/// Fills `cumSeconds` for `events`. `cumSeconds.size()` must equal `events.size()`.
/// The one place a segment's duration is integrated.
void computeCumulativeSeconds(std::span<const TempoEvent> events,
                              std::span<double> cumSeconds) noexcept;

} // namespace adx::core
