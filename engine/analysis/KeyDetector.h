// adx-thread: main
//
// Key detection (phase_10.md §3 step 2): Krumhansl-Schmuckler. A chroma profile is
// correlated with the Krumhansl-Kessler major and minor profiles in all twelve
// rotations, and the best of the 24 is the key. The runners-up are kept, with their
// scores, so the UI can show confidence rather than assert (§10's honest 70 %).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/analysis/Chroma.h"

namespace adx::analysis {

enum class Mode : std::uint8_t { Major, Minor };

struct Key {
    /// 0 = C ... 11 = B.
    std::uint8_t tonic{0};
    Mode mode{Mode::Major};

    [[nodiscard]] friend bool operator==(const Key&, const Key&) noexcept = default;
};

struct KeyCandidate {
    Key key;
    /// Pearson correlation with the key's profile, -1..1.
    double score{0.0};
};

struct KeyEstimate {
    /// All 24, best first.
    std::vector<KeyCandidate> ranked;
    /// How far the best is ahead of the runner-up that is not its relative: 0 is a
    /// coin toss, above ~0.1 is clear.
    double confidence{0.0};

    [[nodiscard]] Key best() const noexcept {
        return ranked.empty() ? Key{} : ranked.front().key;
    }
};

[[nodiscard]] KeyEstimate estimateKey(const ChromaVector& profile);
/// With the bass's own profile (Chromagram::bass): a tonic the bass dwells on scores
/// higher, which is what tells a minor key from its relative major.
[[nodiscard]] KeyEstimate estimateKey(const ChromaVector& profile, const ChromaVector& bass);
/// The same against the plain profiles, for chroma counted from notes: a note has no
/// partials, so spreading the profile over them would favour the subdominant.
[[nodiscard]] KeyEstimate estimateKeyOfNotes(const ChromaVector& profile, const ChromaVector& bass);
/// The chromagram's profile and its bass profile, together; plain profiles when it was
/// built from notes.
[[nodiscard]] KeyEstimate estimateKey(const Chromagram& chroma);

/// "C major", "F# minor".
[[nodiscard]] std::string keyName(const Key& key);
/// "C", "C#" ... "B".
[[nodiscard]] const char* pitchClassName(std::uint8_t pitchClass) noexcept;

/// The relative key: C major <-> A minor.
[[nodiscard]] Key relativeOf(const Key& key) noexcept;

/// The seven pitch classes of the key's scale (natural minor for minor), tonic first.
[[nodiscard]] std::array<std::uint8_t, 7> scaleOf(const Key& key) noexcept;

} // namespace adx::analysis
