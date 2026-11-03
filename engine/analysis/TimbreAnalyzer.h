// adx-thread: main
//
// An additive patch from a reference mixdown, using MIDI notes as ground truth (phase_10.md
// §4.3), ported from _archive/src-cpp/src/TimbreAnalyzer.cpp. The notes say where and at
// what pitch the sound is, so instead of guessing blind it measures the mix there:
//
//   harmonics   per note, a Goertzel probe at each of the first 64 multiples of the known
//               fundamental, over a Hann window in the middle of the sustain (past the
//               attack, before the release), normalised so loud and quiet notes vote
//               equally; then the median across notes per harmonic, weighted by length,
//               so a reverb tail or a delay repeat under one note is outvoted
//   envelope    from an RMS envelope (25 ms windows every 5 ms) around each note's
//               edges: attack is onset to peak, sustain the level before note-off over
//               the peak, decay the time from the peak to that level, release the time
//               after note-off to fall 40 dB; medians again
//
// What changed on port: the target is a Phase 4 `Preset` of type `additive` (named
// harmonic.N and env.* parameters) rather than v1's Patch, and the probe reaches 64
// harmonics instead of 16. v1's pitch-band keyframes are gone because the additive has
// one harmonic table; the median replaces v1's weighted mean, which one loud note could
// drag. Audio comes in as a span, so decoding is the caller's.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

#include "engine/preset/Preset.h"

namespace adx::analysis {

inline constexpr std::uint32_t kTimbreHarmonics = 64;

struct TimbreNote {
    double startSeconds{0.0};
    double seconds{0.0};
    std::uint8_t pitch{60};
};

struct TimbreOptions {
    float tuningA4{440.0F};
    /// Shorter notes have no sustain to probe.
    double minimumSeconds{0.15};
    /// The longest notes win: sustained tones give the cleanest snapshots.
    std::uint32_t maximumNotes{48};
    std::string name{"Analyzed"};
    /// Called after each note with progress in 0..1; returning false cancels (nullopt).
    std::function<bool(double)> progress;
};

struct TimbreAnalysis {
    /// Relative to the strongest, which is 1.
    std::array<float, kTimbreHarmonics> harmonics{};
    float attackSeconds{0.01F};
    float decaySeconds{0.2F};
    float sustain{0.7F};
    float releaseSeconds{0.2F};
    std::uint32_t notesMeasured{0};
    /// The same, as an additive preset: harmonics that are not their default, and env.*.
    preset::Preset preset;
};

/// Nullopt when no note is long enough, inside the audio and below Nyquist with at least
/// two harmonics, or when nothing measurable sounds under any of them.
[[nodiscard]] std::optional<TimbreAnalysis> analyzeTimbre(std::span<const float> mono,
                                                          std::uint32_t sampleRate,
                                                          std::span<const TimbreNote> notes,
                                                          const TimbreOptions& options = {});

} // namespace adx::analysis
