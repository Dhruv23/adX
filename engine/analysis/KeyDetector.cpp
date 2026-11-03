// adx-thread: main
#include "engine/analysis/KeyDetector.h"

#include "engine/analysis/ChordDetector.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>
#include <vector>

namespace adx::analysis {

namespace {

// Temperley's (1999) revision of Krumhansl & Kessler's probe-tone profiles, tonic
// first: on chroma of real tones it confuses a key with its dominant less often.
constexpr std::array<double, 12> kMajor{5.0, 2.0, 3.5, 2.0, 4.5, 4.0, 2.0, 4.5, 2.0, 3.5, 1.5, 4.0};
constexpr std::array<double, 12> kMinor{5.0, 2.0, 3.5, 4.5, 2.0, 4.0, 2.0, 4.5, 3.5, 2.0, 1.5, 4.0};
/// The same with the leading tone lowered: natural minor (Aeolian), as pop and dance
/// music mostly use it. Temperley's profile alone reads i-VII-VI-VII as the relative
/// major, because it expects a raised seventh that Aeolian music never plays. A minor
/// key scores the better of the two.
constexpr std::array<double, 12> kNaturalMinor{5.0, 2.0, 3.5, 4.5, 2.0, 4.0,
                                               2.0, 4.5, 3.5, 2.0, 4.0, 1.5};

constexpr std::array<const char*, 12> kNames{"C",  "C#", "D",  "D#", "E",  "F",
                                             "F#", "G",  "G#", "A",  "A#", "B"};

/// The profile as chroma of real tones would show it: each degree's rating spread over
/// its harmonics' pitch classes, as the chord templates are (ChordDetector.cpp).
std::array<double, 12> harmonic(const std::array<double, 12>& profile) {
    constexpr double kThird = 0.55;
    constexpr double kFifth = 0.35;
    std::array<double, 12> out{};
    for (std::size_t i = 0; i < 12; ++i) {
        out.at(i) += profile.at(i);
        out.at((i + 7) % 12) += profile.at(i) * kThird;
        out.at((i + 4) % 12) += profile.at(i) * kFifth;
    }
    return out;
}

double correlation(const ChromaVector& chroma, const std::array<double, 12>& profile,
                   std::uint8_t tonic) {
    double meanA = 0.0;
    double meanB = 0.0;
    for (std::size_t i = 0; i < 12; ++i) {
        meanA += chroma.at(i);
        meanB += profile.at(i);
    }
    meanA /= 12.0;
    meanB /= 12.0;
    double cov = 0.0;
    double varA = 0.0;
    double varB = 0.0;
    for (std::size_t i = 0; i < 12; ++i) {
        // Pitch class i, relative to the tonic, is the profile's degree (i - tonic).
        const double a = chroma.at(i) - meanA;
        const double b = profile.at((i + 12 - tonic) % 12) - meanB;
        cov += a * b;
        varA += a * a;
        varB += b * b;
    }
    if (varA <= 0.0 || varB <= 0.0) {
        return 0.0;
    }
    return cov / std::sqrt(varA * varB);
}

} // namespace

KeyEstimate estimateKey(const ChromaVector& profile) {
    return estimateKey(profile, ChromaVector{});
}

namespace {

KeyEstimate rank(const ChromaVector& profile, const ChromaVector& bass,
                 const std::array<double, 12>& major, const std::array<double, 12>& minor,
                 const std::array<double, 12>& naturalMinor) {
    // Where the bass dwells is the strongest single cue to the tonic: it is what
    // separates a minor key from its relative major, which share every note.
    constexpr double kBassWeight = 0.05;
    const double bassMean = std::accumulate(bass.begin(), bass.end(), 0.0) / 12.0;
    KeyEstimate out;
    for (std::uint8_t tonic = 0; tonic < 12; ++tonic) {
        out.ranked.push_back(KeyCandidate{.key = Key{.tonic = tonic, .mode = Mode::Major},
                                          .score = correlation(profile, major, tonic) +
                                                   (kBassWeight * (bass.at(tonic) - bassMean))});
        out.ranked.push_back(
            KeyCandidate{.key = Key{.tonic = tonic, .mode = Mode::Minor},
                         .score = std::max(correlation(profile, minor, tonic),
                                           correlation(profile, naturalMinor, tonic)) +
                                  (kBassWeight * (bass.at(tonic) - bassMean))});
    }
    std::ranges::stable_sort(
        out.ranked, [](const KeyCandidate& a, const KeyCandidate& b) { return a.score > b.score; });
    const Key best = out.best();
    const Key relative = relativeOf(best);
    for (std::size_t i = 1; i < out.ranked.size(); ++i) {
        if (!(out.ranked[i].key == relative)) {
            out.confidence = out.ranked.front().score - out.ranked[i].score;
            break;
        }
    }
    return out;
}

/// The cadence cue: music tends to start and end on its tonic chord, which is what
/// separates a minor key from its relative major when no leading tone says which.
/// A key whose tonic triad is the first or the last chord of the passage gains
/// kCadenceWeight for each; then the ranking and confidence are recomputed.
KeyEstimate withCadence(KeyEstimate estimate, const Chromagram& chroma) {
    constexpr double kCadenceWeight = 0.04;
    const std::vector<ChordSegment> chords = detectChords(chroma);
    std::vector<Chord> sounding;
    for (const ChordSegment& segment : chords) {
        if (segment.chord.quality != ChordQuality::None) {
            sounding.push_back(segment.chord);
        }
    }
    if (sounding.empty()) {
        return estimate;
    }
    const auto tonicTriad = [](const Key& key) {
        return Chord{.root = key.tonic,
                     .quality =
                         key.mode == Mode::Major ? ChordQuality::Major : ChordQuality::Minor};
    };
    for (KeyCandidate& candidate : estimate.ranked) {
        const Chord tonic = tonicTriad(candidate.key);
        candidate.score += (sounding.front() == tonic ? kCadenceWeight : 0.0) +
                           (sounding.back() == tonic ? kCadenceWeight : 0.0);
    }
    std::ranges::stable_sort(estimate.ranked, [](const KeyCandidate& a, const KeyCandidate& b) {
        return a.score > b.score;
    });
    estimate.confidence = 0.0;
    const Key relative = relativeOf(estimate.best());
    for (std::size_t i = 1; i < estimate.ranked.size(); ++i) {
        if (!(estimate.ranked[i].key == relative)) {
            estimate.confidence = estimate.ranked.front().score - estimate.ranked[i].score;
            break;
        }
    }
    return estimate;
}

} // namespace

KeyEstimate estimateKey(const ChromaVector& profile, const ChromaVector& bass) {
    static const std::array<double, 12> kHarmonicMajor = harmonic(kMajor);
    static const std::array<double, 12> kHarmonicMinor = harmonic(kMinor);
    static const std::array<double, 12> kHarmonicNaturalMinor = harmonic(kNaturalMinor);
    return rank(profile, bass, kHarmonicMajor, kHarmonicMinor, kHarmonicNaturalMinor);
}

KeyEstimate estimateKeyOfNotes(const ChromaVector& profile, const ChromaVector& bass) {
    return rank(profile, bass, kMajor, kMinor, kNaturalMinor);
}

KeyEstimate estimateKey(const Chromagram& chroma) {
    KeyEstimate out = chroma.fromNotes
                          ? estimateKeyOfNotes(profileOf(chroma), bassProfileOf(chroma))
                          : estimateKey(profileOf(chroma), bassProfileOf(chroma));
    return withCadence(std::move(out), chroma);
}

const char* pitchClassName(std::uint8_t pitchClass) noexcept {
    return kNames.at(pitchClass % 12U);
}

std::string keyName(const Key& key) {
    return std::string(pitchClassName(key.tonic)) + (key.mode == Mode::Major ? " major" : " minor");
}

Key relativeOf(const Key& key) noexcept {
    if (key.mode == Mode::Major) {
        return Key{.tonic = static_cast<std::uint8_t>((key.tonic + 9U) % 12U), .mode = Mode::Minor};
    }
    return Key{.tonic = static_cast<std::uint8_t>((key.tonic + 3U) % 12U), .mode = Mode::Major};
}

std::array<std::uint8_t, 7> scaleOf(const Key& key) noexcept {
    constexpr std::array<std::uint8_t, 7> kMajorSteps{0, 2, 4, 5, 7, 9, 11};
    constexpr std::array<std::uint8_t, 7> kMinorSteps{0, 2, 3, 5, 7, 8, 10};
    const auto& steps = key.mode == Mode::Major ? kMajorSteps : kMinorSteps;
    std::array<std::uint8_t, 7> out{};
    for (std::size_t i = 0; i < 7; ++i) {
        out.at(i) = static_cast<std::uint8_t>((key.tonic + steps.at(i)) % 12U);
    }
    return out;
}

} // namespace adx::analysis
