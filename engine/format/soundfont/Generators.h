// adx-thread: main
//
// SoundFont 2.04's units, converted once (phase_12.md §4.7.4, sf2_generator_units).
#pragma once

#include <cstddef>
#include <cstdint>

namespace adx::format::soundfont {

/// Timecents to seconds: 2^(tc / 1200). -32768 (and anything at or below -12000) is 0.
[[nodiscard]] double timecentsToSeconds(double timecents) noexcept;
/// Absolute cents to Hz: 8.176 * 2^(c / 1200).
[[nodiscard]] double absoluteCentsToHz(double cents) noexcept;
/// Centibels to decibels.
[[nodiscard]] constexpr double centibelsToDb(double cb) noexcept {
    return cb / 10.0;
}

/// SF2's generator operators that adX reads (SoundFont 2.04 §8.1.2).
enum class Gen : std::uint16_t {
    StartAddrsOffset = 0,
    EndAddrsOffset = 1,
    StartloopAddrsOffset = 2,
    EndloopAddrsOffset = 3,
    StartAddrsCoarseOffset = 4,
    ModLfoToPitch = 5,
    VibLfoToPitch = 6,
    ModEnvToPitch = 7,
    InitialFilterFc = 8,
    InitialFilterQ = 9,
    ModLfoToFilterFc = 10,
    ModEnvToFilterFc = 11,
    EndAddrsCoarseOffset = 12,
    ModLfoToVolume = 13,
    ChorusEffectsSend = 15,
    ReverbEffectsSend = 16,
    Pan = 17,
    DelayModLfo = 21,
    FreqModLfo = 22,
    DelayVibLfo = 23,
    FreqVibLfo = 24,
    DelayModEnv = 25,
    AttackModEnv = 26,
    HoldModEnv = 27,
    DecayModEnv = 28,
    SustainModEnv = 29,
    ReleaseModEnv = 30,
    KeynumToModEnvHold = 31,
    KeynumToModEnvDecay = 32,
    DelayVolEnv = 33,
    AttackVolEnv = 34,
    HoldVolEnv = 35,
    DecayVolEnv = 36,
    SustainVolEnv = 37,
    ReleaseVolEnv = 38,
    KeynumToVolEnvHold = 39,
    KeynumToVolEnvDecay = 40,
    Instrument = 41,
    KeyRange = 43,
    VelRange = 44,
    StartloopAddrsCoarseOffset = 45,
    Keynum = 46,
    Velocity = 47,
    InitialAttenuation = 48,
    EndloopAddrsCoarseOffset = 50,
    CoarseTune = 51,
    FineTune = 52,
    SampleId = 53,
    SampleModes = 54,
    ScaleTuning = 56,
    ExclusiveClass = 57,
    OverridingRootKey = 58,
    EndOper = 60,
};
inline constexpr std::size_t kGenCount = 61;

/// The default value of each generator (SoundFont 2.04 §8.1.3).
[[nodiscard]] std::int16_t generatorDefault(std::uint16_t gen) noexcept;

/// Generators that may appear at the preset level and add to the instrument's.
[[nodiscard]] bool presetAdditive(std::uint16_t gen) noexcept;

} // namespace adx::format::soundfont
