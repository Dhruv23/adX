// TimbreAnalyzer and TrackCleaner (phase_10.md §6).
//
// The timbre test renders a known additive patch through the real instrument node,
// recovers a preset from that audio and the notes, renders the preset, and compares the
// two third-octave spectra. The cleaner's form corpus is generated: seeded melodies per
// section letter, humanised onsets, laid out in a known form.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "engine/analysis/TimbreAnalyzer.h"
#include "engine/analysis/TrackCleaner.h"
#include "engine/core/Time.h"
#include "engine/dsp/Fft.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

namespace {

using namespace adx::analysis;
using adx::tests::instrumentParamIndex;
using adx::tests::instrumentParams;

constexpr std::uint32_t kRate = 48000;

/// 31 third-octave band levels in dB, 20 Hz to 20 kHz.
std::vector<double> thirdOctaves(const std::vector<float>& mono) {
    constexpr std::size_t kSize = 8192;
    adx::dsp::Fft fft;
    fft.prepare(kSize);
    std::vector<adx::dsp::Complex> work(kSize);
    std::vector<double> power((kSize / 2) + 1, 0.0);
    for (std::size_t start = 0; start + kSize < mono.size(); start += kSize / 2) {
        for (std::size_t n = 0; n < kSize; ++n) {
            const double w = 0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                                   static_cast<double>(kSize - 1)));
            work[n] = adx::dsp::Complex{static_cast<float>(mono[start + n] * w), 0.0F};
        }
        fft.forward(work);
        for (std::size_t k = 0; k < power.size(); ++k) {
            power[k] += std::norm(work[k]);
        }
    }
    std::vector<double> levels;
    const double binHz = static_cast<double>(kRate) / kSize;
    for (int band = 0; band < 31; ++band) {
        const double centre = 1000.0 * std::pow(2.0, (band - 17) / 3.0);
        const double low = centre * std::pow(2.0, -1.0 / 6.0);
        const double high = centre * std::pow(2.0, 1.0 / 6.0);
        double sum = 0.0;
        for (std::size_t k = 1; k < power.size(); ++k) {
            const double f = static_cast<double>(k) * binHz;
            if (f >= low && f < high) {
                sum += power[k];
            }
        }
        levels.push_back(10.0 * std::log10(sum + 1e-30));
    }
    return levels;
}

struct Played {
    std::vector<float> mono;
    std::vector<TimbreNote> notes;
};

/// The additive with `params`, playing one note per pitch: 1.2 s on, 0.8 s off.
Played play(const std::vector<float>& params, const std::vector<std::uint8_t>& pitches) {
    constexpr std::uint32_t kOn = kRate * 6 / 5;
    constexpr std::uint32_t kStep = kRate * 2;
    std::vector<adx::graph::BlockEvent> events;
    Played out;
    for (std::size_t i = 0; i < pitches.size(); ++i) {
        const auto at = static_cast<std::uint32_t>(i) * kStep;
        const auto id = static_cast<std::uint32_t>(i + 1);
        events.push_back(adx::tests::noteOn(at, id, pitches[i]));
        events.push_back(adx::tests::noteOff(at + kOn, id, pitches[i]));
        out.notes.push_back({.startSeconds = static_cast<double>(at) / kRate,
                             .seconds = static_cast<double>(kOn) / kRate,
                             .pitch = pitches[i]});
    }
    const auto node = adx::tests::preparedInstrument("additive");
    const adx::tests::InstrumentRun run = adx::tests::runInstrument(
        *node, events, static_cast<std::uint32_t>(pitches.size()) * kStep, params);
    out.mono = run.left;
    return out;
}

/// Seeded melody for one section letter: 16th-note onsets over `ticks`, pitches in C
/// major around middle C.
std::vector<CleanNote> melody(char letter, std::int64_t ticks) {
    std::mt19937 random(static_cast<std::uint32_t>(letter) * 7919U);
    constexpr std::int64_t kSixteenth = adx::core::kPpq / 4;
    constexpr std::array<int, 7> kScale{0, 2, 4, 5, 7, 9, 11};
    std::vector<CleanNote> out;
    for (std::int64_t t = 0; t < ticks; t += kSixteenth) {
        if (random() % 3 == 0) {
            const int degree = static_cast<int>(random() % 7);
            out.push_back(
                {.start = t,
                 .length = kSixteenth * (1 + static_cast<std::int64_t>(random() % 2)),
                 .pitch = static_cast<std::uint8_t>(60 + kScale[static_cast<std::size_t>(degree)]),
                 .velocity = 100});
        }
    }
    return out;
}

/// `form`'s sections laid end to end, every onset nudged by up to ±`jitter` ticks.
std::vector<CleanNote> song(const std::string& form, std::int64_t phraseTicks, std::int64_t jitter,
                            std::uint32_t seed) {
    std::mt19937 random(seed);
    std::uniform_int_distribution<std::int64_t> nudge(-jitter, jitter);
    std::vector<CleanNote> out;
    for (std::size_t i = 0; i < form.size(); ++i) {
        for (CleanNote n : melody(form[i], phraseTicks)) {
            n.start =
                std::max<std::int64_t>(0, n.start + (static_cast<std::int64_t>(i) * phraseTicks) +
                                              (jitter > 0 ? nudge(random) : 0));
            out.push_back(n);
        }
    }
    return out;
}

constexpr std::int64_t kPhrase = adx::core::kPpq * 16;
constexpr std::int64_t kGrid = adx::core::kPpq / 4;

} // namespace

TEST_CASE("timbre_analyzer_recovers_patch", "[analysis]") {
    const std::string type = "additive";
    std::vector<float> truth = instrumentParams(type);
    const std::vector<std::pair<int, float>> harmonics{
        {1, 1.0F}, {2, 0.55F}, {3, 0.35F}, {4, 0.25F}, {5, 0.15F}, {7, 0.1F}, {9, 0.06F}};
    for (const auto& [k, level] : harmonics) {
        truth[instrumentParamIndex(type, "harmonic." + std::to_string(k))] = level;
    }
    truth[instrumentParamIndex(type, "env.attack")] = 0.03F;
    truth[instrumentParamIndex(type, "env.decay")] = 0.15F;
    truth[instrumentParamIndex(type, "env.sustain")] = 0.8F;
    truth[instrumentParamIndex(type, "env.release")] = 0.25F;

    const std::vector<std::uint8_t> pitches{48, 52, 55, 60, 64, 67, 57, 62};
    const Played reference = play(truth, pitches);
    const auto measured = analyzeTimbre(reference.mono, kRate, reference.notes);
    REQUIRE(measured.has_value());
    const TimbreAnalysis analysis = measured.value_or(TimbreAnalysis{});
    CHECK(analysis.notesMeasured == pitches.size());
    CHECK(analysis.preset.type == "additive");
    for (const auto& [k, level] : harmonics) {
        CHECK_THAT(analysis.harmonics[static_cast<std::size_t>(k - 1)],
                   Catch::Matchers::WithinAbs(level, 0.03));
    }
    CHECK(analysis.harmonics[5] < 0.01F); // harmonic 6 was never there

    // The preset, applied to a default patch by name, renders the same spectrum.
    std::vector<float> recovered = instrumentParams(type);
    for (const adx::project::ParamValue& p : analysis.preset.params) {
        recovered[instrumentParamIndex(type, p.name)] = static_cast<float>(p.value);
    }
    const Played rendered = play(recovered, pitches);
    const std::vector<double> want = thirdOctaves(reference.mono);
    const std::vector<double> got = thirdOctaves(rendered.mono);
    const double loudest = *std::ranges::max_element(want);
    int compared = 0;
    for (std::size_t band = 0; band < want.size(); ++band) {
        if (want[band] > loudest - 50.0) {
            INFO("band " << band << " want " << want[band] << " got " << got[band]);
            CHECK(std::abs(want[band] - got[band]) <= 1.5);
            ++compared;
        }
    }
    CHECK(compared >= 8);
    CHECK_THAT(analysis.sustain, Catch::Matchers::WithinAbs(0.8, 0.08));
}

TEST_CASE("timbre_analyzer_declines_without_usable_notes", "[analysis]") {
    const std::vector<float> silence(std::size_t{kRate} * 2, 0.0F);
    const std::vector<TimbreNote> shortNote{{.startSeconds = 0.1, .seconds = 0.05, .pitch = 60}};
    CHECK_FALSE(analyzeTimbre(silence, kRate, shortNote).has_value());
    const std::vector<TimbreNote> silent{{.startSeconds = 0.1, .seconds = 1.0, .pitch = 60}};
    CHECK_FALSE(analyzeTimbre(silence, kRate, silent).has_value());
    const std::vector<TimbreNote> outside{{.startSeconds = 5.0, .seconds = 1.0, .pitch = 60}};
    CHECK_FALSE(analyzeTimbre(silence, kRate, outside).has_value());
}

TEST_CASE("track_cleaner_stages_pure", "[analysis]") {
    const std::vector<CleanNote> notes = song("ABABB", kPhrase, 120, 3);
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization) - the snapshot is the point.
    const std::vector<CleanNote> copy = notes;

    CHECK(segmentPhrases(notes, kPhrase) == segmentPhrases(notes, kPhrase));
    std::vector<Phrase> a = segmentPhrases(notes, kPhrase);
    std::vector<Phrase> b = a;
    CHECK(clusterPhrases(a, kGrid) == clusterPhrases(b, kGrid));
    CHECK(a == b);
    CHECK(phraseSimilarity(a[0], a[1], kGrid) == phraseSimilarity(a[0], a[1], kGrid));
    CHECK(phraseSimilarity(a[0], a[0], kGrid) == 1.0F);
    CHECK(blueprintFor("ABABB") == blueprintFor("ABABB"));

    FormAnalysis first;
    FormAnalysis second;
    const std::vector<CleanNote> one = cleanNotes(notes, kPhrase, kGrid, &first);
    const std::vector<CleanNote> two = cleanNotes(notes, kPhrase, kGrid, &second);
    CHECK(one == two);
    CHECK(first == second);
    CHECK(notes == copy); // the input is untouched
    CHECK(masterSection(first, 'A', kGrid) == masterSection(second, 'A', kGrid));
    // Every rebuilt note is on the grid.
    for (const CleanNote& n : one) {
        CHECK(n.start % kGrid == 0);
        CHECK(n.length % kGrid == 0);
    }
}

TEST_CASE("track_cleaner_form_detection", "[analysis]") {
    struct Case {
        std::string form;
        std::string name;
    };
    const std::vector<Case> corpus{
        {.form = "AABA", .name = "Song Form (AABA)"}, {.form = "ABAB", .name = "Verse-Chorus"},
        {.form = "ABABCB", .name = "Verse-Chorus"},   {.form = "ABACA", .name = "Rondo"},
        {.form = "ABA", .name = "Ternary"},           {.form = "AAAA", .name = "Strophic"},
        {.form = "AABB", .name = "Binary"},           {.form = "ABACABA", .name = "Rondo"},
        {.form = "ABABCAB", .name = "Verse-Chorus"},
    };
    std::uint32_t seed = 1;
    for (const Case& c : corpus) {
        for (const std::int64_t jitter : {std::int64_t{0}, std::int64_t{150}, std::int64_t{350}}) {
            const FormAnalysis analysis =
                analyzeForm(song(c.form, kPhrase, jitter, seed++), kPhrase, kGrid);
            INFO(c.form << " jitter " << jitter << " -> " << analysis.observedForm);
            CHECK(analysis.observedForm == c.form);
            CHECK(analysis.formName == c.name);
            CHECK_FALSE(analysis.usedBlueprint);
        }
    }

    // A dropped bridge is recovered: ABABB is not a known form, ABABCB is.
    const FormAnalysis dropped = analyzeForm(song("ABABB", kPhrase, 100, 99), kPhrase, kGrid);
    CHECK(dropped.observedForm == "ABABB");
    CHECK(dropped.blueprint == "ABABCB");
    CHECK(dropped.target == "ABABCB");
    CHECK(dropped.usedBlueprint);
    CHECK(dropped.formName == "Verse-Chorus");
    // ...and the rebuild has six sections, the invented C being A up a fourth.
    const std::vector<CleanNote> rebuilt = rebuild(dropped, kGrid);
    const std::vector<CleanNote> a = masterSection(dropped, 'A', kGrid);
    std::vector<CleanNote> c;
    for (const CleanNote& n : rebuilt) {
        if (n.start >= 4 * kPhrase && n.start < 5 * kPhrase) {
            c.push_back({.start = n.start - (4 * kPhrase),
                         .length = n.length,
                         .pitch = n.pitch,
                         .velocity = n.velocity});
        }
    }
    REQUIRE(c.size() == a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(c[i].pitch == a[i].pitch + 5);
        CHECK(c[i].start == a[i].start);
    }

    // Markers at each change of section.
    const FormAnalysis aaba = analyzeForm(song("AABA", kPhrase, 0, 5), kPhrase, kGrid);
    const std::vector<FormMarker> markers = formMarkers(aaba);
    REQUIRE(markers.size() == 3);
    CHECK(markers[0].tick == 0);
    CHECK(markers[0].label == 'A');
    CHECK(markers[1].tick == 2 * kPhrase);
    CHECK(markers[1].label == 'B');
    CHECK(markers[2].tick == 3 * kPhrase);
}
