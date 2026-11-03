// adx-thread: main
//
// Chord detection (phase_10.md §3 step 2): chroma, template matching, and Viterbi
// smoothing.
//
//   templates   for every root, nine qualities - major, minor, diminished, augmented,
//               sus2, sus4, dominant 7, major 7, minor 7 - as binary pitch-class masks
//               with the root weighted a little higher; plus "no chord" (flat)
//   frames      each chroma frame's cosine similarity to every template is its
//               emission score
//   smoothing   a Viterbi pass over 109 states that pays `changePenalty` for every
//               change of chord, so one noisy frame does not become a chord - the
//               difference between a chord chart and a flicker
//
// Segments come back merged: one per run of the same chord.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/analysis/Chroma.h"

namespace adx::analysis {

enum class ChordQuality : std::uint8_t {
    Major,
    Minor,
    Diminished,
    Augmented,
    Sus2,
    Sus4,
    Dominant7,
    Major7,
    Minor7,
    /// Silence, or nothing chord-like.
    None,
};

struct Chord {
    std::uint8_t root{0};
    ChordQuality quality{ChordQuality::None};

    [[nodiscard]] friend bool operator==(const Chord&, const Chord&) noexcept = default;
};

struct ChordSegment {
    Chord chord;
    std::size_t firstFrame{0};
    /// Exclusive.
    std::size_t endFrame{0};
    /// Mean emission score over the segment, 0..1.
    float confidence{0.0F};
};

struct ChordOptions {
    /// Log-probability cost of a chord change between two frames.
    double changePenalty{0.35};
    /// A frame whose best template scores below this is "no chord".
    float noChordBelow{0.55F};
    /// Templates spread over each tone's partials, for chroma of audio. detectChords
    /// turns it off by itself for a chromagram built from notes.
    bool harmonicTemplates{true};
};

/// The chord (or none) of each frame, unsmoothed: the best template.
[[nodiscard]] Chord bestChordOf(const ChromaVector& frame, const ChordOptions& options = {});
/// Every frame's cosine similarity to `chord`'s template.
[[nodiscard]] float chordScore(const ChromaVector& frame, const Chord& chord,
                               bool harmonicTemplate = true) noexcept;

[[nodiscard]] std::vector<ChordSegment> detectChords(const Chromagram& chroma,
                                                     const ChordOptions& options = {});

/// "C", "Am", "Bdim", "Caug", "Dsus2", "Gsus4", "G7", "Cmaj7", "Am7", "N".
[[nodiscard]] std::string chordName(const Chord& chord);
/// The chord's pitch classes, root first.
[[nodiscard]] std::vector<std::uint8_t> chordTones(const Chord& chord);

} // namespace adx::analysis
