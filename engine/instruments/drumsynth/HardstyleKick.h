// adx-thread: main
//
// The archived hardstyle kick generator, absorbed (phase_4.md §4.7, §5).
//
// In iteration one "the hardstyle kick generator" was two things welded into UI code
// (_archive/src-cpp/src/SampleBrowser.cpp, GenerateHardstyleKicks): a sound - a kick
// sample run through RubberBand - and a placement rule - a kick every `spacing` beats
// over the melody's span, pitch-shifted to the root of whichever melody note covers
// that beat, wrapped into +-6 semitones so the kick never strays an octave. Here:
//
//   the sound      is a preset lineage of the DrumSynth's kick model: the pitch drop,
//                  the distortion stage and the sub reinforcement are kick parameters,
//                  and hardstyleKickParams() is the patch (shipped as
//                  packs/stakillaz/hardstyle-kick-ds.adxpreset). Key tracking is on,
//                  so a note's pitch *is* the shift RubberBand used to apply.
//   the placement  is hardstyleKickNotes(), the generator's loop as a pure function
//                  of the melody, returning notes rather than mutating a sequencer.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Time.h"

namespace adx::instruments {

/// The patch, as (parameter name, value) pairs over the DrumSynth's defaults.
[[nodiscard]] std::span<const std::pair<std::string_view, float>> hardstyleKickParams() noexcept;

struct MelodyNote {
    core::Ticks start;
    core::Ticks length;
    std::uint8_t pitch{60};
};

struct KickNote {
    core::Ticks start;
    core::Ticks length;
    std::uint8_t pitch{60};
};

/// A kick every `spacing` over the melody's span (whole beats, rounded outward), or
/// over `fallbackBars` bars of 4/4 when the melody is empty. Each kick's pitch is
/// `rootKey` plus the interval from the melody's first note to the latest-starting
/// note covering that position, wrapped into -5..+6 semitones - as the archive did.
[[nodiscard]] std::vector<KickNote> hardstyleKickNotes(std::span<const MelodyNote> melody,
                                                       core::Ticks spacing, int fallbackBars,
                                                       std::uint8_t rootKey);

/// The archive's WrapSemitones: an interval folded into -5..+6.
[[nodiscard]] int wrapSemitones(int semitones) noexcept;

} // namespace adx::instruments
