// Chroma, key and chord detection, and chord suggestion (phase_10.md §6).
//
// The corpora are synthesized, not recorded: progressions in known keys, each chord a
// set of harmonic tones (six partials, decaying) in a seeded voicing and inversion, with
// a bass note. They test the detectors against ground truth that is exact by
// construction; real recordings are harder, and that measurement is phase_10.md §11's
// open item, not a claim made here.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "engine/analysis/ChordDetector.h"
#include "engine/analysis/ChordSuggest.h"
#include "engine/analysis/Chroma.h"
#include "engine/analysis/KeyDetector.h"

namespace {

using namespace adx::analysis;

constexpr std::uint32_t kRate = 22050;

void addTone(std::vector<float>& out, std::size_t start, std::size_t length, double midi,
             float level) {
    const double hz = 440.0 * std::exp2((midi - 69.0) / 12.0);
    for (int partial = 1; partial <= 6; ++partial) {
        const double f = hz * partial;
        if (f > kRate / 2.0) {
            break;
        }
        const double amplitude = level / std::pow(partial, 1.2);
        for (std::size_t i = 0; i < length && start + i < out.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            const double envelope = std::min(1.0, t * 50.0) * std::exp(-t * 0.8);
            out[start + i] +=
                static_cast<float>(amplitude * envelope * std::sin(2.0 * std::numbers::pi * f * t));
        }
    }
}

/// One chord for `seconds`: its tones in an octave starting at a seeded inversion, the
/// root an octave below.
void addChord(std::vector<float>& out, double startSeconds, double seconds, const Chord& chord,
              std::mt19937& random) {
    const auto start = static_cast<std::size_t>(startSeconds * kRate);
    const auto length = static_cast<std::size_t>(seconds * kRate);
    std::vector<std::uint8_t> tones = chordTones(chord);
    const std::size_t inversion = random() % tones.size();
    const int base = 48 + static_cast<int>(random() % 7);
    for (std::size_t i = 0; i < tones.size(); ++i) {
        const std::uint8_t pc = tones[(i + inversion) % tones.size()];
        int midi = base + ((pc - base % 12 + 12) % 12);
        if (i > 0 && midi < base + 2) {
            midi += 12;
        }
        addTone(out, start, length, midi, 0.15F);
    }
    addTone(out, start, length, 36 + chord.root, 0.2F); // the bass
}

struct Progression {
    Key key;
    std::vector<Chord> chords;
};

/// A progression in `key` from scale degrees.
Progression progression(const Key& key, const std::vector<int>& degrees) {
    Progression out{.key = key, .chords = {}};
    for (const int degree : degrees) {
        out.chords.push_back(diatonicTriad(key, degree));
    }
    return out;
}

std::vector<float> render(const Progression& p, double secondsPerChord, std::mt19937& random) {
    std::vector<float> out(
        static_cast<std::size_t>(secondsPerChord * static_cast<double>(p.chords.size()) * kRate),
        0.0F);
    for (std::size_t i = 0; i < p.chords.size(); ++i) {
        addChord(out, secondsPerChord * static_cast<double>(i), secondsPerChord, p.chords[i],
                 random);
    }
    return out;
}

std::uint8_t argmax(const ChromaVector& chroma) {
    return static_cast<std::uint8_t>(std::ranges::max_element(chroma) - chroma.begin());
}

} // namespace

TEST_CASE("chroma_pitch_class", "[analysis][harmony]") {
    for (int midi = 40; midi <= 84; ++midi) {
        std::vector<float> tone(kRate, 0.0F);
        addTone(tone, 0, tone.size(), midi, 0.5F);
        const ChromaVector profile = profileOf(chromagram(tone, kRate));
        INFO("midi " << midi);
        CHECK(argmax(profile) == midi % 12);
    }
    // A tone between two semitones splits between them; tuning moves it back.
    std::vector<float> sharp(kRate, 0.0F);
    addTone(sharp, 0, sharp.size(), 69.5, 0.5F);
    const ChromaVector split = profileOf(chromagram(sharp, kRate));
    CHECK(std::abs(split.at(9) - split.at(10)) < 0.15F);
    ChromaOptions tuning;
    tuning.tuningCents = 50.0F;
    const ChromaVector tuned = profileOf(chromagram(sharp, kRate, tuning));
    CHECK(argmax(tuned) == 9);
    // Silence is all zeros, not noise.
    const std::vector<float> silent(kRate, 0.0F);
    for (const ChromaVector& frame : chromagram(silent, kRate).frames) {
        CHECK(std::ranges::all_of(frame, [](float v) { return v == 0.0F; }));
    }
}

TEST_CASE("key_detector_corpus", "[analysis][harmony]") {
    // Forty progressions: every major and minor key at least once, in idioms of both.
    const std::vector<std::vector<int>> majorIdioms{
        {1, 4, 5, 1}, {1, 5, 6, 4}, {1, 6, 4, 5}, {2, 5, 1, 1}, {1, 4, 1, 5, 1}};
    const std::vector<std::vector<int>> minorIdioms{
        {1, 4, 5, 1}, {1, 6, 3, 7}, {1, 7, 6, 7}, {1, 4, 7, 3}, {1, 6, 4, 5, 1}};
    std::mt19937 random(2026);
    int exact = 0;
    int relative = 0;
    int total = 0;
    for (int i = 0; i < 40; ++i) {
        const bool major = i % 2 == 0;
        const Key key{.tonic = static_cast<std::uint8_t>((i * 7) % 12),
                      .mode = major ? Mode::Major : Mode::Minor};
        const auto& idioms = major ? majorIdioms : minorIdioms;
        Progression p = progression(key, idioms[static_cast<std::size_t>(i / 2) % idioms.size()]);
        if (!major) {
            // Minor-key music raises the leading tone at the dominant: harmonic minor V.
            for (Chord& chord : p.chords) {
                if (chord.root == (key.tonic + 7) % 12) {
                    chord.quality = ChordQuality::Major;
                }
            }
        }
        const std::vector<float> audio = render(p, 1.5, random);
        const KeyEstimate estimate = estimateKey(chromagram(audio, kRate));
        ++total;
        if (estimate.best() == key) {
            ++exact;
            ++relative;
        } else if (estimate.best() == relativeOf(key)) {
            ++relative;
        }
        UNSCOPED_INFO(keyName(key) << " -> " << keyName(estimate.best()));
    }
    const double exactRate = static_cast<double>(exact) / total;
    const double relativeRate = static_cast<double>(relative) / total;
    INFO("exact " << exactRate << ", with relatives " << relativeRate);
    // phase_10.md §6: > 85 % exact, > 95 % with relatives. Measured 92.5 % and 100 %
    // since minor keys also score against natural minor (§11); the misses are i-iv-VII-
    // III, which ends on the relative major's tonic. This is the synthetic corpus; the
    // recorded one is tools/fetch_key_corpus.py's, measured in phase_10.md §11.
    CHECK(exactRate > 0.85);
    CHECK(relativeRate > 0.95);
}

TEST_CASE("key_from_notes", "[analysis][harmony]") {
    // G major's scale, the tonic and dominant longest.
    std::vector<ChromaNote> notes;
    for (const int pitch : {67, 69, 71, 72, 74, 76, 78, 79}) {
        notes.push_back(ChromaNote{.pitch = static_cast<std::uint8_t>(pitch),
                                   .duration = pitch % 12 == 7 || pitch % 12 == 2 ? 2.0 : 1.0,
                                   .velocity = 1.0F});
    }
    const KeyEstimate estimate = estimateKey(profileOfNotes(notes));
    CHECK(keyName(estimate.best()) == "G major");
    CHECK(estimate.confidence > 0.0);
    CHECK(estimate.ranked.size() == 24);
}

TEST_CASE("chord_detector_corpus", "[analysis][harmony]") {
    std::mt19937 random(7);
    std::size_t correct = 0;
    std::size_t frames = 0;
    const std::vector<Chord> vocabulary{{.root = 0, .quality = ChordQuality::Major},
                                        {.root = 9, .quality = ChordQuality::Minor},
                                        {.root = 5, .quality = ChordQuality::Major},
                                        {.root = 7, .quality = ChordQuality::Dominant7},
                                        {.root = 2, .quality = ChordQuality::Minor7},
                                        {.root = 11, .quality = ChordQuality::Diminished},
                                        {.root = 4, .quality = ChordQuality::Minor},
                                        {.root = 0, .quality = ChordQuality::Major7},
                                        {.root = 7, .quality = ChordQuality::Sus4},
                                        {.root = 2, .quality = ChordQuality::Sus2},
                                        {.root = 8, .quality = ChordQuality::Augmented},
                                        {.root = 3, .quality = ChordQuality::Major}};
    for (int trial = 0; trial < 6; ++trial) {
        Progression p{.key = Key{}, .chords = {}};
        for (int i = 0; i < 8; ++i) {
            p.chords.push_back(vocabulary[random() % vocabulary.size()]);
        }
        const double seconds = 1.0;
        const std::vector<float> audio = render(p, seconds, random);
        const Chromagram chroma = chromagram(audio, kRate);
        const std::vector<ChordSegment> segments = detectChords(chroma);
        for (const ChordSegment& segment : segments) {
            for (std::size_t f = segment.firstFrame; f < segment.endFrame; ++f) {
                const auto chordIndex = static_cast<std::size_t>(chroma.timeOf(f, 8192) / seconds);
                if (chordIndex >= p.chords.size()) {
                    continue;
                }
                ++frames;
                if (segment.chord == p.chords[chordIndex]) {
                    ++correct;
                }
            }
        }
    }
    const double accuracy = static_cast<double>(correct) / static_cast<double>(frames);
    INFO("frame accuracy " << accuracy << " over " << frames << " frames");
    // 81.7 % before the chroma's floor subtraction, 92.1 % with it (phase_10.md §11).
    CHECK(accuracy > 0.85);
}

TEST_CASE("chord_detector_smooths_and_names", "[analysis][harmony]") {
    CHECK(chordName(Chord{0, ChordQuality::Major}) == "C");
    CHECK(chordName(Chord{9, ChordQuality::Minor}) == "Am");
    CHECK(chordName(Chord{7, ChordQuality::Dominant7}) == "G7");
    CHECK(chordName(Chord{6, ChordQuality::Diminished}) == "F#dim");
    CHECK(chordName(Chord{}) == "N");
    // C held for 2 s with one frame of noise in it: still one segment.
    std::mt19937 random(3);
    Progression const p{.key = Key{}, .chords = {Chord{.root = 0, .quality = ChordQuality::Major}}};
    std::vector<float> audio = render(p, 2.0, random);
    const std::size_t mid = audio.size() / 2;
    for (std::size_t i = mid; i < mid + 1500; ++i) {
        audio[i] += static_cast<float>(random() % 1000) / 2000.0F - 0.25F;
    }
    const std::vector<ChordSegment> segments = detectChords(chromagram(audio, kRate));
    REQUIRE(segments.size() == 1);
    CHECK(segments.front().chord == Chord{0, ChordQuality::Major});
    // Silence is "no chord".
    const std::vector<float> silent(std::size_t{kRate} * 2, 0.0F);
    const auto none = detectChords(chromagram(silent, kRate));
    REQUIRE(none.size() == 1);
    CHECK(none.front().chord.quality == ChordQuality::None);
}

TEST_CASE("chord_suggest_in_key", "[analysis][harmony]") {
    const Key c{.tonic = 0, .mode = Mode::Major};
    const auto afterG = suggestChords(c, Chord{.root = 7, .quality = ChordQuality::Major});
    // Diatonic, all seven, and the dominant resolves home first.
    REQUIRE(afterG.size() >= 7);
    CHECK(afterG.front().chord == Chord{0, ChordQuality::Major});
    const auto scale = scaleOf(c);
    for (const ChordSuggestion& s : afterG) {
        if (s.degree > 0) {
            CHECK(std::ranges::find(scale, s.chord.root) != scale.end());
        }
    }
    const auto afterF = suggestChords(c, Chord{.root = 5, .quality = ChordQuality::Major});
    CHECK(afterF.front().chord == Chord{7, ChordQuality::Major}); // IV -> V
    CHECK(romanNumeral(c, Chord{9, ChordQuality::Minor}) == "vi");
    CHECK(romanNumeral(c, Chord{11, ChordQuality::Diminished}) == "vii\xC2\xB0");
    CHECK(romanNumeral(c, Chord{10, ChordQuality::Major}) == "bVII");
    CHECK(romanNumeral(c, Chord{1, ChordQuality::Major}).empty());
    const Key a{.tonic = 9, .mode = Mode::Minor};
    CHECK(diatonicTriad(a, 3) == Chord{0, ChordQuality::Major}); // A minor's III is C
    CHECK(keyName(relativeOf(c)) == "A minor");
}
