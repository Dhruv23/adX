// adx-thread: main
//
// What chord might come next (phase_10.md §2): key-aware, ranked suggestions. The
// diatonic triads of the key, scored by a small table of common-practice and pop
// motion between scale degrees (V to I, IV to V, vi to IV...), plus the key's
// borrowed chords at a lower score. A suggestion, never a rule: every diatonic chord is
// offered, only the order changes.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "engine/analysis/ChordDetector.h"
#include "engine/analysis/KeyDetector.h"

namespace adx::analysis {

struct ChordSuggestion {
    Chord chord;
    /// Scale degree 1..7, or 0 for a borrowed chord.
    int degree{0};
    /// Relative plausibility, 0..1, best first.
    double score{0.0};
};

/// Suggestions after `previous` (none: what a progression might start with).
[[nodiscard]] std::vector<ChordSuggestion> suggestChords(const Key& key,
                                                         const std::optional<Chord>& previous);

/// The diatonic triad on scale degree `degree` (1..7) of `key`.
[[nodiscard]] Chord diatonicTriad(const Key& key, int degree);

/// "I", "ii", "V", "vii°", "bVII" - the Roman numeral of `chord` in `key`, or "" for a
/// chord outside the key and its usual borrowings.
[[nodiscard]] std::string romanNumeral(const Key& key, const Chord& chord);

} // namespace adx::analysis
