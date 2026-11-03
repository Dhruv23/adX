// Destructive edits, stretching, warp markers and freeze (phase_8.md §3.2-§3.4, §5).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "engine/clip/ClipEdit.h"
#include "engine/clip/ClipRenderCache.h"
#include "engine/clip/Freeze.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/audio/AudioFileLoader.h"
#include "engine/project/commands/ArrangeCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/clip/ClipFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace fs = std::filesystem;
using adx::tests::ClipDir;

namespace {

constexpr std::uint32_t kRate = 48000;

std::vector<float> render(const adx::project::Project& project, std::uint64_t frames) {
    adx::render::OfflineRenderOptions options;
    options.frames = frames;
    adx::render::RenderStats stats;
    std::vector<float> out = adx::render::renderOffline(project, options, stats);
    REQUIRE(stats.error.empty());
    return out;
}

std::unique_ptr<adx::tests::Loaded> audioProject(const fs::path& file, const std::string& keys,
                                                 const std::string& children = "") {
    return adx::tests::loadText(
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\nTRACK 1\n"
        "  AUDIO \"" +
        file.generic_string() + "\" 0:0:0 " + keys + "\n" + children +
        "\n[MIXER]\nINSERT 1 name=\"Master\"\n");
}

adx::project::PlaylistItem& onlyItem(adx::project::Project& project) {
    return project.playlist.tracks.front().items.front();
}

} // namespace

TEST_CASE("clip_destructive_is_undoable", "[clip][edits]") {
    ClipDir const dir;
    std::vector<float> source = adx::tests::stereoNoise(kRate / 2, 9);
    for (float& s : source) {
        s = (s * 0.3F) + 0.1F; // an offset for DC removal to find, quiet for normalize
    }
    const fs::path file = dir.write("take.wav", source, kRate);
    const auto plain = audioProject(file, "");
    const std::vector<float> original = render(plain->project, kRate);
    for (const char* edit : {"reverse", "normalize:-1", "dc", "gain:-6", "trim:-20"}) {
        INFO(edit);
        const auto edited = audioProject(file, std::string("edit=") + edit);
        const std::vector<float> changed = render(edited->project, kRate);
        CHECK(changed != original);
        // Undo is removing the edit from the list: a command like any other.
        adx::project::PlaylistItem without = onlyItem(edited->project);
        std::get<adx::project::AudioClipRef>(without.content).edits.clear();
        const adx::core::PlaylistTrackId track = edited->project.playlist.tracks.front().id;
        edited->stack.execute(
            std::make_unique<adx::project::SetPlaylistItem>(track, without.id, without, track),
            edited->project);
        REQUIRE(render(edited->project, kRate) == original);
        REQUIRE(edited->stack.undo(edited->project));
        REQUIRE(render(edited->project, kRate) == changed);
    }
}

TEST_CASE("clip_destructive_never_writes_source", "[clip][edits]") {
    ClipDir const dir;
    const fs::path file = dir.write("take.wav", adx::tests::stereoNoise(kRate / 2, 3), kRate);
    std::uint64_t before = 0;
    REQUIRE(adx::format::hashFile(file, before));
    const auto stamp = fs::last_write_time(file);
    const auto edited =
        audioProject(file, "edit=reverse,normalize:0,dc,gain:3,trim:-40 stretch=1.5");
    static_cast<void>(render(edited->project, kRate));
    std::uint64_t after = 0;
    REQUIRE(adx::format::hashFile(file, after));
    CHECK(after == before);
    CHECK(fs::last_write_time(file) == stamp);
}

TEST_CASE("stretch_quality", "[clip][stretch]") {
    ClipDir const dir;
    const fs::path file =
        dir.write("sine.wav", adx::tests::stereoSine(kRate, kRate, 1000.0), kRate);
    const auto stretched = audioProject(file, "stretch=2");
    const std::vector<float> out = render(stretched->project, 3 * static_cast<std::size_t>(kRate));
    // Twice as long, and still a 1 kHz tone: within a cent (0.058 %).
    const double hz = adx::tests::measuredFrequency(out, kRate, kRate / 2, (3 * kRate) / 2);
    CHECK_THAT(hz, Catch::Matchers::WithinAbs(1000.0, 0.577));
    std::size_t last = 0;
    for (std::size_t f = 0; f < 3 * static_cast<std::size_t>(kRate); ++f) {
        if (std::abs(out[f * 2]) > 0.05F) {
            last = f;
        }
    }
    CHECK(last > (2 * kRate) - 2000);
    CHECK(last < (2 * kRate) + 100);

    // Pitch without time: a fifth up, same length.
    const auto pitched = audioProject(file, "pitch=7");
    const std::vector<float> up = render(pitched->project, kRate);
    CHECK_THAT(adx::tests::measuredFrequency(up, kRate, kRate / 8, (kRate * 7) / 8),
               Catch::Matchers::WithinRel(1000.0 * std::exp2(7.0 / 12.0), 0.000577));
}

TEST_CASE("warp_markers_piecewise_map", "[clip][warp]") {
    ClipDir const dir;
    // Clicks at 0.5 s and 1.5 s of a 2 s file.
    std::vector<float> clicks(static_cast<std::size_t>(2 * kRate) * 2, 0.0F);
    for (const std::size_t at : {kRate / 2, (kRate * 3) / 2}) {
        for (std::size_t i = 0; i < 64; ++i) {
            clicks[(at + i) * 2] = clicks[((at + i) * 2) + 1] = 0.8F;
        }
    }
    const fs::path file = dir.write("clicks.wav", clicks, kRate);
    // Source 0.5 s at beat 1 (0.5 s at 120), source 1.5 s at beat 5 (2.5 s): the second
    // segment is stretched to twice its length, the first plays as it was.
    const auto loaded = audioProject(file, "", "    WARP 0.5 0:1:0\n    WARP 1.5 1:1:0\n");
    const adx::project::PlaylistItem& item = onlyItem(loaded->project);
    const auto& audio = std::get<adx::project::AudioClipRef>(item.content);
    REQUIRE(audio.warps.size() == 2);

    // The map, by hand: the clip start is an implicit marker (source 0 -> 0 s), so the
    // first segment plays at its own speed; markers at 0.5 -> 0.5 s and 1.5 -> 2.5 s.
    const adx::clip::ClipRenderKey key =
        adx::clip::renderKeyFor(loaded->project, item, kRate, adx::format::SamplePool::global());
    REQUIRE(key.warp.size() == 2);
    CHECK_THAT(key.warp[0].first, Catch::Matchers::WithinAbs(0.5, 1e-12));
    CHECK_THAT(key.warp[0].second, Catch::Matchers::WithinAbs(0.5, 1e-12));
    CHECK_THAT(key.warp[1].first, Catch::Matchers::WithinAbs(1.5, 1e-12));
    CHECK_THAT(key.warp[1].second, Catch::Matchers::WithinAbs(2.5, 1e-12));
    CHECK_THAT(adx::clip::renderedStartSeconds(loaded->project, item),
               Catch::Matchers::WithinAbs(0.0, 1e-12));

    // And the clicks land there, to within ten milliseconds: RubberBand follows a key
    // frame map a process block at a time (Stretch.cpp). A phase vocoder's output is
    // never exactly silent between clicks, so a click is where the level first passes
    // half its height.
    const std::vector<float> out = render(loaded->project, 4 * static_cast<std::size_t>(kRate));
    const auto clickFrom = [&out](std::size_t from) {
        for (std::size_t f = from; f * 2 < out.size(); ++f) {
            if (std::abs(out[f * 2]) > 0.4F) {
                return f;
            }
        }
        return out.size() / 2;
    };
    const std::size_t first = clickFrom(kRate / 4);
    const std::size_t second = clickFrom(kRate + (kRate / 2));
    CHECK(std::abs(static_cast<double>(first) - (kRate * 0.5)) < kRate * 0.010);
    CHECK(std::abs(static_cast<double>(second) - (kRate * 2.5)) < kRate * 0.010);
}

TEST_CASE("warp_cache_key", "[clip][warp]") {
    ClipDir const dir;
    const fs::path file = dir.write("loop.wav", adx::tests::stereoNoise(kRate, 2), kRate);
    const auto loaded = audioProject(file, "", "    WARP 0.25 0:0:3840\n    WARP 0.75 0:2:0\n");
    adx::project::PlaylistItem& item = onlyItem(loaded->project);
    auto& audio = std::get<adx::project::AudioClipRef>(item.content);
    adx::format::SamplePool& pool = adx::format::SamplePool::global();
    const std::string base = adx::clip::renderKeyFor(loaded->project, item, kRate, pool).text();

    // Every warp parameter, and every other property that changes the buffer, makes a
    // different key - for the stretched buffer and, since the playlist keys its peaks by
    // the same text, for the waveform too.
    std::vector<std::string> keys{base};
    const auto changed = [&](auto&& mutate) {
        adx::project::PlaylistItem copy = item;
        mutate(copy, std::get<adx::project::AudioClipRef>(copy.content));
        keys.push_back(adx::clip::renderKeyFor(loaded->project, copy, kRate, pool).text());
    };
    changed([](auto&, auto& a) { a.warps[0].source += 0.01; });
    changed([](auto&, auto& a) { a.warps[1].at.value += 10; });
    changed([](auto&, auto& a) { a.warps.pop_back(); });
    changed([](auto&, auto& a) { a.pitchSemitones = 1.0F; });
    changed([](auto&, auto& a) { a.reverse = true; });
    changed([](auto&, auto& a) {
        a.edits.push_back({.kind = adx::project::EditKind::Gain, .amount = -3.0F});
    });
    for (std::size_t i = 0; i < keys.size(); ++i) {
        for (std::size_t j = i + 1; j < keys.size(); ++j) {
            INFO(i << " vs " << j);
            CHECK(keys[i] != keys[j]);
        }
    }
    // What does not change the buffer keeps it: where the clip is placed, and its slip -
    // a warped clip's markers are measured from its own start, so slipping it moves the
    // read position over the same warped audio (ClipEdit.h, renderedStartSeconds).
    adx::project::PlaylistItem moved = item;
    moved.start.value += adx::core::kPpq * 8;
    static_cast<void>(audio);
    CHECK(adx::clip::renderKeyFor(loaded->project, moved, kRate, pool).text() == base);
    adx::project::PlaylistItem slipped = item;
    slipped.sourceOffset.value += 100;
    CHECK(adx::clip::renderKeyFor(loaded->project, slipped, kRate, pool).text() == base);
    CHECK(adx::clip::renderedStartSeconds(loaded->project, slipped) !=
          adx::clip::renderedStartSeconds(loaded->project, item));
}

TEST_CASE("freeze_is_transparent", "[clip][freeze]") {
    ClipDir const dir;
    const fs::path path = dir.path() / "song.adx";
    const std::string text =
        adx::tests::readFile(adx::tests::repoRoot() / "tests/golden/routing_sends.adx");
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(text, project, stack, diagnostics,
                      adx::format::ParseOptions{.baseDirectory = dir.path().string()});
    REQUIRE_FALSE(diagnostics.hasErrors());
    const std::vector<float> live = render(project, 10 * static_cast<std::size_t>(kRate));
    const adx::project::Project before = project;

    for (const adx::project::Channel& channel : before.channels) {
        INFO(channel.name);
        const adx::clip::FreezeResult frozen = adx::clip::freezeChannel(project, stack, channel.id);
        INFO(frozen.error);
        REQUIRE(frozen.ok);
        REQUIRE_FALSE(project.find(channel.id)->frozen.empty());
    }
    // Every channel frozen, and the mix is the same to the last bit.
    REQUIRE(render(project, 10 * static_cast<std::size_t>(kRate)) == live);
    (void)path;
}

TEST_CASE("freeze_is_reversible", "[clip][freeze]") {
    ClipDir const dir;
    const std::string text =
        adx::tests::readFile(adx::tests::repoRoot() / "tests/golden/chord_steal.adx");
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(text, project, stack, diagnostics,
                      adx::format::ParseOptions{.baseDirectory = dir.path().string()});
    const adx::project::Project before = project;
    const std::vector<float> live = render(project, 6 * static_cast<std::size_t>(kRate));
    const adx::core::ChannelId channel = project.channels.front().id;
    REQUIRE(adx::clip::freezeChannel(project, stack, channel).ok);

    adx::clip::unfreezeChannel(project, stack, channel);
    CHECK(project.channels == before.channels);
    REQUIRE(render(project, 6 * static_cast<std::size_t>(kRate)) == live);
    // And undoing both steps comes back to the same place.
    REQUIRE(stack.undo(project));
    REQUIRE(stack.undo(project));
    CHECK(project.channels == before.channels);
}
