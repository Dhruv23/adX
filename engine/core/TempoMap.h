// Tempo, meter, and the exact mapping between musical and wall-clock time.
//
// Phase 3's sample-accurate scheduling is built directly on this, so the one
// property that matters more than speed is that toSamples and toTicks are
// inverses. A numerically integrated ramp does not have that property - the two
// directions disagree by a fraction of a sample per segment, which accumulates
// into audible drift on a long project - so ramps integrate in closed form
// (phase_2.md §4.2).
//
// Main thread only. It holds std::vector, it rebuilds its prefix sums on edit, and
// nothing on the audio callback path touches it.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

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

class TempoMap {
public:
    /// A new map is 120 bpm in 4/4. Both lists always hold an event at tick 0 -
    /// every lookup below assumes it, and removing it is not an operation.
    TempoMap();

    /// Inserts a tempo change, or replaces the one already at `at`. The bpm is
    /// clamped into [kMinBpm, kMaxBpm] rather than rejected: this is called by the
    /// parser, which has already emitted a diagnostic about the out-of-range value
    /// and still needs a map it can hand to the renderer.
    void setTempo(Ticks at, double bpm, bool ramp);

    /// Removes the tempo change at `at`. Refuses to remove the one at tick 0.
    bool removeTempo(Ticks at);

    void setMeter(Ticks at, std::uint16_t numerator, std::uint16_t denominator);
    bool removeMeter(Ticks at);

    [[nodiscard]] std::span<const TempoEvent> tempoEvents() const noexcept {
        return m_tempo;
    }
    [[nodiscard]] std::span<const MeterEvent> meterEvents() const noexcept {
        return m_meter;
    }

    [[nodiscard]] double bpmAt(Ticks at) const noexcept;
    [[nodiscard]] MeterEvent meterAt(Ticks at) const noexcept;

    /// Wall-clock seconds elapsed from tick 0 to `at`. Negative ticks extrapolate
    /// backwards through the first segment's tempo, which is what a count-in needs.
    [[nodiscard]] double secondsAt(Ticks at) const noexcept;
    [[nodiscard]] Ticks ticksAtSeconds(double seconds) const noexcept;

    [[nodiscard]] Samples toSamples(Ticks at, std::uint32_t sampleRate) const noexcept;
    [[nodiscard]] Ticks toTicks(Samples at, std::uint32_t sampleRate) const noexcept;

    [[nodiscard]] BarBeatTick toBarBeat(Ticks at) const noexcept;
    [[nodiscard]] Ticks fromBarBeat(BarBeatTick position) const noexcept;

    [[nodiscard]] friend bool operator==(const TempoMap& lhs, const TempoMap& rhs) noexcept {
        return lhs.m_tempo == rhs.m_tempo && lhs.m_meter == rhs.m_meter;
    }

private:
    /// Recomputes the prefix sums. Called on every edit, never during render.
    void rebuild();

    [[nodiscard]] std::size_t tempoIndexAt(Ticks at) const noexcept;
    [[nodiscard]] std::size_t meterIndexAt(Ticks at) const noexcept;

    std::vector<TempoEvent> m_tempo;
    std::vector<MeterEvent> m_meter;

    /// Seconds elapsed at the start of tempo segment i.
    ///
    /// Seconds rather than the samples phase_2.md §4.2 named: samples would make the
    /// table depend on the sample rate, so an offline render at 96 kHz and a live
    /// stream at 48 kHz would need two of them and could disagree about where a
    /// segment boundary lands. Seconds are the rate-independent quantity and the
    /// rounding to frames happens once, at the end, in toSamples.
    std::vector<double> m_cumSeconds;

    /// Bars elapsed before meter segment i begins. A meter change that does not land
    /// on a bar line still consumes the whole partial bar, so bar numbers stay
    /// strictly increasing and toBarBeat stays invertible.
    std::vector<std::int64_t> m_cumBars;
};

} // namespace adx::core
