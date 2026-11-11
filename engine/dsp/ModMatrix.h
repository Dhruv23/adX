// The modulation matrix's vocabulary: sources, destinations, and how far a slot at
// full amount moves each destination (phase_12.md §4.3).
//
// One recurring pattern is behind most of what the packs need - brightness follows
// loudness, pitch dives at the note's start, vibrato arrives late - and each is
// "source X drives destination Y by this much". A fixed knob per pairing is how
// iteration one grew thirty one-off fields; eight slots of (source, destination,
// amount) cover them all.
//
// A destination an instrument has no use for is ignored by that instrument: the FM
// operator levels mean nothing to the VA. The enums are append-only, since a slot's
// source and destination are stored in project files as their numbers.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace adx::dsp {

enum class ModSource : std::uint8_t {
    None,
    ModEnv1,
    ModEnv2,
    Lfo1,
    Lfo2,
    /// 0..1.
    Velocity,
    /// -1..1 over the keyboard, 0 at middle C.
    Key,
    /// The instrument's `mod.wheel` and `mod.pressure` parameters, 0..1: automatable
    /// and MIDI-learnable like any other.
    ModWheel,
    Aftertouch,
    /// One value per note in -1..1, from the voice's identity: the same note gets the
    /// same value offline and in realtime.
    Random,
    /// 1 at the note's start, falling to 0 over `pitchEnv.time`.
    PitchEnv,
};
inline constexpr std::uint8_t kModSourceCount = 11;

enum class ModDest : std::uint8_t {
    None,
    Pitch,
    Osc1Pitch,
    Osc2Pitch,
    PulseWidth,
    Cutoff,
    Resonance,
    Level,
    Pan,
    Drive,
    UnisonDetune,
    WtPosition,
    WarpAmount,
    FmOp1,
    FmOp2,
    FmOp3,
    FmOp4,
    FmOp5,
    FmOp6,
    FmFeedback,
    Exciter,
    ModalDecay,
};
inline constexpr std::uint8_t kModDestCount = 22;

/// What a slot at amount 1 with its source at 1 adds: semitones for the pitches,
/// octaves for the cutoff, cents for the detune, the destination's own 0..1 otherwise.
inline constexpr std::array<float, kModDestCount> kModDestScale{
    0.0F, 48.0F, 48.0F, 48.0F, 0.45F, 8.0F, 1.0F, 1.0F, 1.0F, 4.0F, 100.0F,
    1.0F, 1.0F,  1.0F,  1.0F,  1.0F,  1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};

inline constexpr std::size_t kModSlots = 8;

struct ModSlot {
    ModSource source{ModSource::None};
    ModDest dest{ModDest::None};
    float amount{0.0F};
};

/// One frame's modulation: each destination's offset in its own units. Level is a gain
/// factor instead, 1 when nothing moves it.
struct ModFrame {
    std::array<float, kModDestCount> value{};

    void clear() noexcept {
        value.fill(0.0F);
        value[static_cast<std::size_t>(ModDest::Level)] = 1.0F;
    }
    [[nodiscard]] float of(ModDest dest) const noexcept {
        return value[static_cast<std::size_t>(dest)];
    }
    /// Adds a slot's contribution. Level is a tremolo: at amount a the gain swings
    /// between 1 - a and 1 for a bipolar source, about 1 - a/2.
    void add(ModDest dest, float source, float amount) noexcept {
        const auto at = static_cast<std::size_t>(dest);
        if (dest == ModDest::Level) {
            value[at] *= 1.0F - (std::abs(amount) * 0.5F) + (0.5F * (source * amount));
        } else {
            value[at] += kModDestScale[at] * (source * amount);
        }
    }
};

/// The neutral frame, for a voice nothing modulates.
[[nodiscard]] inline const ModFrame& neutralModFrame() noexcept {
    static const ModFrame kNeutral = [] {
        ModFrame frame;
        frame.clear();
        return frame;
    }();
    return kNeutral;
}

/// The tempo-synced LFO lengths, in whole notes: `lfoN.sync` 1..12. 0 is free (Hz).
inline constexpr std::array<double, 13> kLfoSyncWholes{
    0.0,      1.0 / 64, 1.0 / 32, 1.0 / 24, 1.0 / 16, 1.0 / 12, 1.0 / 8,
    3.0 / 16, 1.0 / 4,  1.0 / 2,  1.0,      2.0,      4.0};

} // namespace adx::dsp
