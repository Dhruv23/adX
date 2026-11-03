// adx-thread: main
//
// "Clean and structurize" for a messy extracted melody (phase_10.md §4.3), ported from
// _archive/src-cpp/src/TrackCleaner.cpp with its staging kept: each stage is a pure
// function of its input, testable alone and usable alone.
//
//   1 segmentation   fixed-length windows of the timeline, notes by onset
//   2 similarity     tolerant onset matching (±1 grid slot), rhythm as a Jaccard score,
//                    pitch as the share of aligned pairs agreeing on one transpose
//   3 clustering     greedy first match, loosened until there are at most six sections
//   4 form matching  Strophic, Binary, Ternary, Rondo, Verse-Chorus, AABA, or Freeform
//   5 blueprint      a Markov model over standard forms plus an edit-distance prior,
//                    which recovers a dropped section ("ABABB" -> "ABABCB")
//   6 master         the most representative phrase of a label, rebased and quantized
//   7 rebuild        the blueprint, one master per letter on consecutive windows
//
// Form detection alone is what places arrangement markers (formMarkers below), which
// the archived version never surfaced.
//
// What changed on port: time is ticks (core::kPpq per beat) instead of float beats, so
// a rebuilt note lands exactly on the grid; the notes are a small value type the app
// converts to and from a pattern's notes.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adx::analysis {

struct CleanNote {
    std::int64_t start{0};
    std::int64_t length{0};
    std::uint8_t pitch{60};
    std::uint8_t velocity{100};

    [[nodiscard]] friend bool operator==(const CleanNote&, const CleanNote&) noexcept = default;
};

struct Phrase {
    std::uint32_t index{0};
    /// Absolute: notes keep their own start here; only a master is rebased.
    std::int64_t start{0};
    std::vector<CleanNote> notes;
    /// 'A', 'B', ... from clusterPhrases; '?' before.
    char label{'?'};

    [[nodiscard]] friend bool operator==(const Phrase&, const Phrase&) noexcept = default;
};

struct FormAnalysis {
    std::int64_t phraseTicks{0};
    std::vector<Phrase> phrases;
    /// Canonical: the first distinct letter is A ("ABABB").
    std::string observedForm;
    std::string formName;
    std::string blueprint;
    /// The form the rebuild follows: the observed one when it is a known form,
    /// otherwise the blueprint.
    std::string target;
    bool usedBlueprint{false};

    [[nodiscard]] friend bool operator==(const FormAnalysis&,
                                         const FormAnalysis&) noexcept = default;
};

// Stage 1. Windows of `phraseTicks` up to the last note's end; none empty at the end.
[[nodiscard]] std::vector<Phrase> segmentPhrases(const std::vector<CleanNote>& notes,
                                                 std::int64_t phraseTicks);
// Stage 2. 0..1; both silent is 1, one silent 0. `grid` in ticks.
[[nodiscard]] float phraseSimilarity(const Phrase& a, const Phrase& b, std::int64_t grid);
// Stage 3. Labels in place; returns the threshold it settled at.
float clusterPhrases(std::vector<Phrase>& phrases, std::int64_t grid, float threshold = 0.7F);
[[nodiscard]] std::string formString(const std::vector<Phrase>& phrases);
[[nodiscard]] std::string canonicalForm(const std::string& form);
// Stage 4.
[[nodiscard]] std::string matchKnownForm(const std::string& form);
// Stage 5.
[[nodiscard]] std::string blueprintFor(const std::string& observedForm);
// Stages 1-5.
[[nodiscard]] FormAnalysis analyzeForm(const std::vector<CleanNote>& notes,
                                       std::int64_t phraseTicks, std::int64_t grid);
// Stage 6. Empty when no phrase carries `label`.
[[nodiscard]] std::vector<CleanNote> masterSection(const FormAnalysis& analysis, char label,
                                                   std::int64_t grid);
// Stage 7. A letter with no phrase of its own is section A up a fourth.
[[nodiscard]] std::vector<CleanNote> rebuild(const FormAnalysis& analysis, std::int64_t grid);

/// Stages 1-7.
[[nodiscard]] std::vector<CleanNote> cleanNotes(const std::vector<CleanNote>& notes,
                                                std::int64_t phraseTicks, std::int64_t grid,
                                                FormAnalysis* analysis = nullptr);

/// Where a section starts and what it is: "A", "B"... at each change of label in the
/// observed form, for arrangement markers.
struct FormMarker {
    std::int64_t tick{0};
    char label{'A'};
};
[[nodiscard]] std::vector<FormMarker> formMarkers(const FormAnalysis& analysis);

} // namespace adx::analysis
