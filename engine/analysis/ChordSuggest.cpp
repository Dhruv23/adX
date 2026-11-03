// adx-thread: main
#include "engine/analysis/ChordSuggest.h"

#include <algorithm>
#include <array>
#include <string>

namespace adx::analysis {

namespace {

/// The triad quality on each degree of the major and natural-minor scales.
constexpr std::array<ChordQuality, 7> kMajorQualities{
    ChordQuality::Major, ChordQuality::Minor, ChordQuality::Minor,     ChordQuality::Major,
    ChordQuality::Major, ChordQuality::Minor, ChordQuality::Diminished};
constexpr std::array<ChordQuality, 7> kMinorQualities{
    ChordQuality::Minor, ChordQuality::Diminished, ChordQuality::Major, ChordQuality::Minor,
    ChordQuality::Minor, ChordQuality::Major,      ChordQuality::Major};

/// How usual motion from degree row to degree column is (1-based degrees, index 0 is
/// "start of a progression"). Tuned from common practice and pop: strong
/// dominant-tonic, predominant-dominant and descending-fifth motion score high.
constexpr std::array<std::array<double, 7>, 8> kMotion{{
    //  I    ii   iii  IV   V    vi   vii
    {1.0, 0.3, 0.2, 0.6, 0.5, 0.7, 0.1}, // start
    {0.2, 0.5, 0.3, 0.9, 0.9, 0.8, 0.2}, // from I
    {0.3, 0.1, 0.2, 0.4, 1.0, 0.3, 0.5}, // from ii
    {0.3, 0.3, 0.1, 0.7, 0.4, 1.0, 0.1}, // from iii
    {0.8, 0.6, 0.2, 0.1, 1.0, 0.5, 0.3}, // from IV
    {1.0, 0.2, 0.3, 0.5, 0.1, 0.8, 0.2}, // from V
    {0.4, 0.8, 0.3, 1.0, 0.6, 0.1, 0.1}, // from vi
    {1.0, 0.1, 0.4, 0.1, 0.3, 0.3, 0.1}, // from vii
}};

const std::array<ChordQuality, 7>& qualitiesOf(Mode mode) {
    return mode == Mode::Major ? kMajorQualities : kMinorQualities;
}

int degreeOf(const Key& key, const Chord& chord) {
    const auto scale = scaleOf(key);
    for (std::size_t i = 0; i < 7; ++i) {
        if (scale.at(i) == chord.root && qualitiesOf(key.mode).at(i) == chord.quality) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

/// The chords borrowed most often: bVII and iv in major, V (harmonic minor) in minor.
std::vector<Chord> borrowed(const Key& key) {
    if (key.mode == Mode::Major) {
        return {Chord{.root = static_cast<std::uint8_t>((key.tonic + 10U) % 12U),
                      .quality = ChordQuality::Major},
                Chord{.root = static_cast<std::uint8_t>((key.tonic + 5U) % 12U),
                      .quality = ChordQuality::Minor}};
    }
    return {Chord{.root = static_cast<std::uint8_t>((key.tonic + 7U) % 12U),
                  .quality = ChordQuality::Major}};
}

} // namespace

Chord diatonicTriad(const Key& key, int degree) {
    const auto index = static_cast<std::size_t>(std::clamp(degree, 1, 7) - 1);
    return Chord{.root = scaleOf(key).at(index), .quality = qualitiesOf(key.mode).at(index)};
}

std::vector<ChordSuggestion> suggestChords(const Key& key, const std::optional<Chord>& previous) {
    const int from = previous ? degreeOf(key, *previous) : 0;
    // A borrowed or foreign chord as the previous one: treat it like the start.
    const auto& row = kMotion.at(static_cast<std::size_t>(from));
    std::vector<ChordSuggestion> out;
    for (int degree = 1; degree <= 7; ++degree) {
        out.push_back(ChordSuggestion{.chord = diatonicTriad(key, degree),
                                      .degree = degree,
                                      .score = row.at(static_cast<std::size_t>(degree - 1))});
    }
    for (const Chord& chord : borrowed(key)) {
        out.push_back(ChordSuggestion{.chord = chord, .degree = 0, .score = 0.25});
    }
    std::ranges::stable_sort(
        out, [](const ChordSuggestion& a, const ChordSuggestion& b) { return a.score > b.score; });
    return out;
}

std::string romanNumeral(const Key& key, const Chord& chord) {
    constexpr std::array<const char*, 7> kNumerals{"I", "II", "III", "IV", "V", "VI", "VII"};
    const int degree = degreeOf(key, chord);
    if (degree > 0) {
        std::string numeral = kNumerals.at(static_cast<std::size_t>(degree - 1));
        if (chord.quality == ChordQuality::Minor || chord.quality == ChordQuality::Diminished) {
            std::ranges::transform(numeral, numeral.begin(),
                                   [](char c) { return static_cast<char>(c - 'A' + 'a'); });
        }
        if (chord.quality == ChordQuality::Diminished) {
            numeral += "\xC2\xB0"; // °
        }
        return numeral;
    }
    const auto interval = static_cast<std::uint8_t>((chord.root + 12U - key.tonic) % 12U);
    if (key.mode == Mode::Major && interval == 10 && chord.quality == ChordQuality::Major) {
        return "bVII";
    }
    if (key.mode == Mode::Major && interval == 5 && chord.quality == ChordQuality::Minor) {
        return "iv";
    }
    if (key.mode == Mode::Minor && interval == 7 && chord.quality == ChordQuality::Major) {
        return "V";
    }
    return {};
}

} // namespace adx::analysis
