// The genre suites (phase_10.md §4.6, §6): generation is seeded and deterministic, a
// generated track loads and renders clean, and each suite's seed-1 track is a committed
// golden fixture in tests/golden/ (whose render hash golden_corpus_stable then holds).
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <random>
#include <set>
#include <string>

#include "engine/analysis/KeyDetector.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/generate/GenreSuite.h"
#include "engine/generate/RiffGenerator.h"
#include "engine/preset/PresetLibrary.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/Env.h"
#include "tests/cpp/render/GoldenHashes.h"

namespace {

using namespace adx::generate;

const adx::preset::PresetLibrary& packs() {
    static const adx::preset::PresetLibrary library = [] {
        adx::preset::PresetLibrary l;
        adx::format::DiagnosticList diagnostics;
        l.scan(adx::preset::packsDirectory(adx::tests::repoRoot()), diagnostics);
        return l;
    }();
    return library;
}

std::string textOf(const GeneratedTrack& track) {
    return adx::format::write(track.project);
}

} // namespace

TEST_CASE("riff_generator_basics", "[generate]") {
    const adx::analysis::Key aMinor{.tonic = 9, .mode = adx::analysis::Mode::Minor};
    CHECK(scalePitch(aMinor, 57, 0) == 57);
    CHECK(scalePitch(aMinor, 57, 2) == 60);  // C
    CHECK(scalePitch(aMinor, 57, 7) == 69);  // the octave
    CHECK(scalePitch(aMinor, 57, -1) == 55); // G below
    const auto chords = progression(aMinor, {1, 6, 3, 7}, 60);
    REQUIRE(chords.size() == 4);
    CHECK(chords[0].pitches.size() == 3);
    for (const Chord& chord : chords) {
        for (const int p : chord.pitches) {
            CHECK(p >= 60 - 2);
            CHECK(p <= 60 + 19);
            // Every tone is in the key.
            const auto scale = adx::analysis::scaleOf(aMinor);
            CHECK(std::ranges::find(scale, static_cast<std::uint8_t>(p % 12)) != scale.end());
        }
    }
    std::mt19937 random(5);
    const auto arp = arpeggio(chords[0], 0, adx::core::kPpq * 4, adx::core::kPpq / 4, 2,
                              ArpMode::Up, 80, 0.5, random);
    CHECK(arp.size() == 16);
    CHECK(arp[0].pitch < arp[1].pitch);
    const auto hits = pulse("X...x...x...x...", 2, adx::core::kPpq * 4, 36, 100, 127, 0.5);
    REQUIRE(hits.size() == 8);
    CHECK(hits[0].velocity == 127);
    CHECK(hits[1].start == adx::core::kPpq);
    // roll() is in range and does not depend on a library distribution.
    std::mt19937 r(1);
    for (int i = 0; i < 1000; ++i) {
        const int v = roll(r, -3, 4);
        CHECK(v >= -3);
        CHECK(v <= 4);
    }
}

TEST_CASE("genre_suite_deterministic", "[generate]") {
    REQUIRE(packs().all().size() >= 15);
    for (const StyleInfo& style : styles()) {
        INFO(style.id);
        StyleOptions options;
        options.seed = 42;
        const std::string a = textOf(generateTrack(style.id, packs(), options));
        const std::string b = textOf(generateTrack(style.id, packs(), options));
        CHECK(a == b);
        options.seed = 43;
        CHECK(textOf(generateTrack(style.id, packs(), options)) != a);
        // Every preset the suite names exists in its pack.
        const GeneratedTrack track = generateTrack(style.id, packs(), options);
        for (const std::string& note : track.notes) {
            CHECK(note.find("not found") == std::string::npos);
        }
        // The template has the same channels and mixer, and no notes.
        const GeneratedTrack bare = styleTemplate(style.id, packs(), options);
        CHECK(bare.project.channels.size() == track.project.channels.size());
        CHECK(bare.project.mixer.inserts.size() == track.project.mixer.inserts.size());
        CHECK(bare.project.patterns.empty());
        CHECK(bare.project.markers.size() == track.project.markers.size());
    }
    CHECK_THROWS_AS(generateTrack("polka", packs()), std::invalid_argument);
}

TEST_CASE("genre_suite_renders", "[generate][render]") {
    for (const StyleInfo& style : styles()) {
        INFO(style.id);
        StyleOptions options;
        options.seed = 1;
        options.bars = 24;
        const GeneratedTrack track = generateTrack(style.id, packs(), options);
        const std::string text = textOf(track);

        // It loads back with no errors, and writes back the same.
        adx::project::Project project;
        adx::project::CommandStack stack;
        adx::format::DiagnosticList diagnostics;
        adx::format::load(text, project, stack, diagnostics);
        REQUIRE_FALSE(diagnostics.hasErrors());
        CHECK(adx::format::write(project) == text);

        // The key the suite chose is the key the detector hears in its notes.
        std::vector<adx::analysis::ChromaNote> notes;
        for (const auto& pattern : project.patterns) {
            for (const auto& clip : pattern.noteClips) {
                const auto* channel = project.find(clip.channel);
                if (channel != nullptr && channel->name == "Kick") {
                    continue;
                }
                for (const auto& n : clip.notes) {
                    notes.push_back({.pitch = n.pitch,
                                     .duration = static_cast<double>(n.length.value),
                                     .velocity = 1.0F});
                }
            }
        }
        const auto estimate =
            adx::analysis::estimateKeyOfNotes(adx::analysis::profileOfNotes(notes), {});
        const bool same =
            estimate.best() == track.key || estimate.best() == adx::analysis::relativeOf(track.key);
        CHECK(same);

        // Clean: finite, audible, under the limiter's ceiling.
        adx::render::OfflineRenderOptions render;
        render.frames = static_cast<std::uint64_t>(48000 * 8);
        adx::render::RenderStats stats;
        const std::vector<float> audio = adx::render::renderOffline(project, render, stats);
        REQUIRE(stats.error.empty());
        CHECK(stats.peak > 0.05F);
        CHECK(stats.peak <= 1.0F);
        CHECK(std::ranges::all_of(audio, [](float v) { return std::isfinite(v); }));

        // The seed-1 track is a golden fixture; golden_corpus_stable holds its render.
        const auto path = adx::tests::goldenDirectory() / ("genre_" + style.id + "_seed1.adx");
        if (adx::tests::environment("ADX_UPDATE_GOLDEN")) {
            std::ofstream(path, std::ios::binary) << text;
        }
        std::ifstream in(path, std::ios::binary);
        REQUIRE(in.good());
        const std::string committed{std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>()};
        CHECK(committed == text);
    }
}
