// adx-thread: main
#include "engine/analysis/ChordDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "engine/analysis/KeyDetector.h"

namespace adx::analysis {

namespace {

constexpr std::size_t kQualities = 9;

/// Intervals above the root, per quality.
/// A sounding pitch class as chroma sees it: the note, and what its harmonics add -
/// the third partial a fifth up, the fifth partial a major third up (Gomez 2006). A
/// template without them reads every C major chord as Cmaj7, because E's third
/// partial is B.
void addTone(ChromaVector& chroma, std::uint8_t pitchClass, float weight, bool partials) {
    // Partial strengths as chroma sees them - square-rooted magnitudes, so a third
    // partial a third as loud still weighs about half.
    constexpr float kThird = 0.55F;
    constexpr float kFifth = 0.35F;
    chroma.at(pitchClass) += weight;
    if (!partials) {
        return;
    }
    chroma.at((pitchClass + 7U) % 12U) += weight * kThird;
    chroma.at((pitchClass + 4U) % 12U) += weight * kFifth;
}

const std::array<std::vector<std::uint8_t>, kQualities>& intervals() {
    static const std::array<std::vector<std::uint8_t>, kQualities> kTable{{
        {0, 4, 7},     // major
        {0, 3, 7},     // minor
        {0, 3, 6},     // diminished
        {0, 4, 8},     // augmented
        {0, 2, 7},     // sus2
        {0, 5, 7},     // sus4
        {0, 4, 7, 10}, // dominant 7
        {0, 4, 7, 11}, // major 7
        {0, 3, 7, 10}, // minor 7
    }};
    return kTable;
}

constexpr std::array<const char*, kQualities> kSuffix{"",     "m", "dim",  "aug", "sus2",
                                                      "sus4", "7", "maj7", "m7"};

ChromaVector templateOf(const Chord& chord, bool partials) {
    ChromaVector out{};
    if (chord.quality == ChordQuality::None) {
        out.fill(1.0F);
        return out;
    }
    const auto& steps = intervals().at(static_cast<std::size_t>(chord.quality));
    for (std::size_t i = 0; i < steps.size(); ++i) {
        // The root a little stronger, so C and Am7 (same notes but one) separate.
        addTone(out, static_cast<std::uint8_t>((chord.root + steps[i]) % 12U), i == 0 ? 1.2F : 1.0F,
                partials);
    }
    return out;
}

float cosine(const ChromaVector& a, const ChromaVector& b) noexcept {
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (std::size_t i = 0; i < 12; ++i) {
        dot += static_cast<double>(a.at(i)) * b.at(i);
        na += static_cast<double>(a.at(i)) * a.at(i);
        nb += static_cast<double>(b.at(i)) * b.at(i);
    }
    if (na <= 0.0 || nb <= 0.0) {
        return 0.0F;
    }
    return static_cast<float>(dot / std::sqrt(na * nb));
}

std::vector<Chord> allChords() {
    std::vector<Chord> out;
    for (std::uint8_t root = 0; root < 12; ++root) {
        for (std::size_t q = 0; q < kQualities; ++q) {
            out.push_back(Chord{.root = root, .quality = static_cast<ChordQuality>(q)});
        }
    }
    out.push_back(Chord{.root = 0, .quality = ChordQuality::None});
    return out;
}

/// The emission score of "no chord": what a frame scores against the flat template,
/// lifted so it wins only where no real chord reaches `noChordBelow`.
float noChordScore(const ChromaVector& frame, float floor) noexcept {
    const bool silent = std::ranges::all_of(frame, [](float v) { return v == 0.0F; });
    return silent ? 1.0F : floor;
}

} // namespace

float chordScore(const ChromaVector& frame, const Chord& chord, bool harmonicTemplate) noexcept {
    return cosine(frame, templateOf(chord, harmonicTemplate));
}

Chord bestChordOf(const ChromaVector& frame, const ChordOptions& options) {
    Chord best{};
    float bestScore = noChordScore(frame, options.noChordBelow);
    for (const Chord& chord : allChords()) {
        if (chord.quality == ChordQuality::None) {
            continue;
        }
        const float score = chordScore(frame, chord, options.harmonicTemplates);
        if (score > bestScore) {
            bestScore = score;
            best = chord;
        }
    }
    return best;
}

std::vector<ChordSegment> detectChords(const Chromagram& chroma, const ChordOptions& options) {
    std::vector<ChordSegment> out;
    const std::vector<Chord> states = allChords();
    const std::size_t n = states.size();
    const std::size_t frames = chroma.frames.size();
    if (frames == 0) {
        return out;
    }
    const bool harmonic = options.harmonicTemplates && !chroma.fromNotes;
    // Scores as log-likelihoods, Viterbi with a flat change penalty.
    std::vector<float> emission(frames * n);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::size_t s = 0; s < n; ++s) {
            const float score = states[s].quality == ChordQuality::None
                                    ? noChordScore(chroma.frames[f], options.noChordBelow)
                                    : chordScore(chroma.frames[f], states[s], harmonic);
            emission[(f * n) + s] = score;
        }
    }
    const auto logOf = [](float score) { return std::log(std::max(score, 1e-6F)); };
    std::vector<double> best(n);
    std::vector<double> next(n);
    std::vector<std::uint16_t> from(frames * n);
    for (std::size_t s = 0; s < n; ++s) {
        best[s] = logOf(emission[s]);
    }
    for (std::size_t f = 1; f < frames; ++f) {
        // The best predecessor is either the same state, or the overall best plus the
        // penalty: O(n) per frame rather than O(n^2).
        const auto leader = static_cast<std::size_t>(std::ranges::max_element(best) - best.begin());
        for (std::size_t s = 0; s < n; ++s) {
            const double stay = best[s];
            const double change = best[leader] - options.changePenalty;
            const bool stays = stay >= change;
            next[s] = (stays ? stay : change) + logOf(emission[(f * n) + s]);
            from[(f * n) + s] = static_cast<std::uint16_t>(stays ? s : leader);
        }
        best.swap(next);
    }
    std::vector<std::size_t> path(frames);
    path[frames - 1] = static_cast<std::size_t>(std::ranges::max_element(best) - best.begin());
    for (std::size_t f = frames - 1; f > 0; --f) {
        path[f - 1] = from[(f * n) + path[f]];
    }
    for (std::size_t f = 0; f < frames; ++f) {
        const Chord& chord = states[path[f]];
        const float score = emission[(f * n) + path[f]];
        if (!out.empty() && out.back().chord == chord) {
            ChordSegment& segment = out.back();
            const auto length = static_cast<float>(segment.endFrame - segment.firstFrame);
            segment.confidence = ((segment.confidence * length) + score) / (length + 1.0F);
            segment.endFrame = f + 1;
        } else {
            out.push_back(ChordSegment{
                .chord = chord, .firstFrame = f, .endFrame = f + 1, .confidence = score});
        }
    }
    return out;
}

std::string chordName(const Chord& chord) {
    if (chord.quality == ChordQuality::None) {
        return "N";
    }
    return std::string(pitchClassName(chord.root)) +
           kSuffix.at(static_cast<std::size_t>(chord.quality));
}

std::vector<std::uint8_t> chordTones(const Chord& chord) {
    std::vector<std::uint8_t> out;
    if (chord.quality == ChordQuality::None) {
        return out;
    }
    for (const std::uint8_t step : intervals().at(static_cast<std::size_t>(chord.quality))) {
        out.push_back(static_cast<std::uint8_t>((chord.root + step) % 12U));
    }
    return out;
}

} // namespace adx::analysis
