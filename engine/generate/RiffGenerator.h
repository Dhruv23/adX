// adx-thread: main
//
// Seeded generation over a scale and a rhythm (phase_10.md §4.6): the material the genre
// suites arrange. Every function takes its random source, so the same seed is the same
// notes on every machine and in every build (genre_suite_deterministic): the source is
// std::mt19937, whose output the standard fixes, and no distribution is used whose
// results the standard leaves to the library.
//
//   progression   chords on scale degrees, voiced close around a centre pitch
//   arpeggio      a held chord broken into steps: up, down, up-down, or seeded order
//   melody        a line on the scale that moves mostly by step, rests by density,
//                 lands on chord tones on the beat, and stays inside a range
//   pulse         hits on a step grid from a 16-step template ("x...x...x...x...")
#pragma once

#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

#include "engine/analysis/KeyDetector.h"

namespace adx::generate {

struct GenNote {
    std::int64_t start{0};
    std::int64_t length{0};
    std::uint8_t pitch{60};
    std::uint8_t velocity{100};

    [[nodiscard]] friend bool operator==(const GenNote&, const GenNote&) noexcept = default;
};

/// An integer in [lo, hi], from the generator's raw output: portable, unlike
/// std::uniform_int_distribution.
[[nodiscard]] int roll(std::mt19937& random, int lo, int hi) noexcept;
/// True with probability `p`, the same way.
[[nodiscard]] bool chance(std::mt19937& random, double p) noexcept;

/// The MIDI pitch of scale degree `degree` (0-based, may be negative or above 6) of
/// `key`, in the octave whose tonic is `tonicPitch`.
[[nodiscard]] int scalePitch(const analysis::Key& key, int tonicPitch, int degree) noexcept;

struct Chord {
    /// 1-based scale degree of the root.
    int degree{1};
    std::vector<int> pitches;
};

/// The diatonic triad (or seventh, with `sevenths`) on each degree, inverted to sit
/// close above `centre`. A bass line is the suite's to add.
[[nodiscard]] std::vector<Chord> progression(const analysis::Key& key,
                                             const std::vector<int>& degrees, int centre,
                                             bool sevenths = false);

enum class ArpMode : std::uint8_t { Up, Down, UpDown, Random };

/// `chord` broken into notes every `step` ticks across [start, start + length), over
/// `octaves` octaves.
[[nodiscard]] std::vector<GenNote> arpeggio(const Chord& chord, std::int64_t start,
                                            std::int64_t length, std::int64_t step, int octaves,
                                            ArpMode mode, std::uint8_t velocity, double gate,
                                            std::mt19937& random);

struct MelodySpec {
    analysis::Key key;
    int tonicPitch{60};
    int lowest{55};
    int highest{79};
    std::int64_t start{0};
    std::int64_t length{0};
    std::int64_t step{960};
    /// Chance a step starts a note.
    double density{0.5};
    /// Chance a note moves by a leap rather than a step.
    double leap{0.2};
    std::uint8_t velocity{90};
    /// The chord under each bar (or none): its tones are preferred on the beat.
    const std::vector<Chord>* chords{nullptr};
    std::int64_t barTicks{0};
};

[[nodiscard]] std::vector<GenNote> melody(const MelodySpec& spec, std::mt19937& random);

/// `pattern` is 16 characters per bar of sixteenth steps: 'x' a hit, 'X' an accent,
/// '.' a rest; repeated across `bars`.
[[nodiscard]] std::vector<GenNote> pulse(std::string_view pattern, int bars, std::int64_t barTicks,
                                         std::uint8_t pitch, std::uint8_t velocity,
                                         std::uint8_t accent, double gate);

} // namespace adx::generate
