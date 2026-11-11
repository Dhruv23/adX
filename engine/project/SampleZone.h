// A Sampler zone: which sample plays for which keys and velocities, and how
// (phase_4.md §4.5).
//
// Zones are structure, not parameters: they say which file plays, so they are not
// automatable and a change to them makes a new instrument node (the way a Limiter's
// lookahead does) rather than a knob turn. They live on the InstrumentSpec so a preset
// carries them. The sample is a pool reference (Resources), never a path, so ten zones
// on one file are one decoded buffer.
#pragma once

#include <cstdint>
#include <string_view>

#include "engine/core/Ids.h"

namespace adx::project {

enum class LoopMode : std::uint8_t {
    /// Plays from `start` to the end of the sample, once.
    Off,
    /// Loops [loopStart, loopEnd) for as long as the voice lasts.
    Forward,
    /// Loops forward then backward across [loopStart, loopEnd).
    PingPong,
    /// Loops while the note is held; on release, plays on from the loop to the end.
    Sustain,
    /// Plays through once while held; on release, jumps to loopEnd and plays the tail.
    Release,
};
inline constexpr std::size_t kLoopModeCount = 5;

[[nodiscard]] const char* toString(LoopMode mode) noexcept;
[[nodiscard]] bool loopModeFromString(std::string_view name, LoopMode& out) noexcept;

/// When a zone plays: at the note-on, or at the note-off (a piano's or harpsichord's
/// release noise).
enum class ZoneTrigger : std::uint8_t { Attack, Release };

struct SampleZone {
    core::SampleId sample;
    std::uint8_t keyLow{0};
    std::uint8_t keyHigh{127};
    /// The key at which the sample plays at its recorded pitch.
    std::uint8_t rootKey{60};
    std::uint8_t velocityLow{1};
    std::uint8_t velocityHigh{127};
    /// Where playback starts, in the sample's own frames.
    std::uint32_t start{0};
    /// Exclusive: where playback stops, in the sample's own frames. Zero means the end
    /// of the sample. A Slicer's slice is a zone from `start` to `end`.
    std::uint32_t end{0};
    LoopMode loop{LoopMode::Off};
    std::uint32_t loopStart{0};
    /// Exclusive. Zero means the end of the sample.
    std::uint32_t loopEnd{0};
    /// Frames of equal-power crossfade at the loop seam.
    std::uint32_t crossfade{0};
    float tuneCents{0.0F};
    float gainDb{0.0F};
    float pan{0.0F};
    /// Zones sharing a non-zero group take turns: the group's counter picks the zone
    /// whose index is next. Seeded per snapshot, never rand() (phase_4.md §4.5).
    std::uint8_t roundRobinGroup{0};
    std::uint8_t roundRobinIndex{0};

    // --- phase_12.md §4.7.1. Every default leaves the zone as it was. ---

    /// A keyswitch: keys switchLow..switchHigh select an articulation and make no
    /// sound, and this zone plays only while the last one pressed was `switchKey`.
    /// 0..0 means the zone is in no keyswitch group.
    std::uint8_t switchLow{0};
    std::uint8_t switchHigh{0};
    std::uint8_t switchKey{0};
    ZoneTrigger trigger{ZoneTrigger::Attack};
    /// A release zone's level falls by this many dB for each second the note was held.
    float rtDecay{0.0F};
    /// An equal-power crossfade between dynamic layers: the zone fades in across
    /// xfadeInLow..xfadeInHigh and out across xfadeOutLow..xfadeOutHigh of the
    /// sampler's `xfade.source` (velocity, or the `dynamics` parameter). The defaults
    /// are no fade.
    std::uint8_t xfadeInLow{0};
    std::uint8_t xfadeInHigh{0};
    std::uint8_t xfadeOutLow{127};
    std::uint8_t xfadeOutHigh{127};
    /// Choke groups: a note in `group` cuts every sounding zone whose `offBy` is that
    /// group (a closed hat cutting the open one). 0 is no group.
    std::uint16_t group{0};
    std::uint16_t offBy{0};

    [[nodiscard]] bool hasSwitch() const noexcept {
        return switchLow != 0 || switchHigh != 0;
    }
    [[nodiscard]] bool hasXfade() const noexcept {
        return xfadeInHigh != 0 || xfadeOutLow != 127;
    }

    [[nodiscard]] bool matches(std::uint8_t key, std::uint8_t velocity) const noexcept {
        return key >= keyLow && key <= keyHigh && velocity >= velocityLow &&
               velocity <= velocityHigh;
    }

    [[nodiscard]] friend bool operator==(const SampleZone&, const SampleZone&) noexcept = default;
};

} // namespace adx::project
