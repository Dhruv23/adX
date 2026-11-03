// Audio clips playing (phase_8.md §3.2, §5): slip, trim, fades, crossfades, clip
// envelopes, rate conversion, and the property everything else leans on - that a clip
// renders the same samples whatever the block size.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include "engine/clip/ClipEdit.h"
#include "engine/core/Curve.h"
#include "engine/geometry/ClipPeakCache.h"
#include "engine/geometry/PlaylistGeometry.h"
#include "engine/project/commands/ArrangeCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/clip/ClipFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace fs = std::filesystem;
using adx::tests::ClipDir;
using adx::tests::loadText;

namespace {

constexpr std::uint32_t kRate = 48000;
/// At 120 bpm a beat is half a second.
constexpr std::uint64_t kBeat = kRate / 2;

std::vector<float> render(const adx::project::Project& project, std::uint64_t frames,
                          std::uint32_t block = 256) {
    adx::render::OfflineRenderOptions options;
    options.sampleRate = kRate;
    options.blockFrames = block;
    options.frames = frames;
    adx::render::RenderStats stats;
    std::vector<float> out = adx::render::renderOffline(project, options, stats);
    REQUIRE(stats.error.empty());
    CHECK(stats.violations == 0);
    return out;
}

std::string project(const std::string& items, const std::string& trackKeys = "") {
    return "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\nTRACK 1" + trackKeys +
           "\n" + items + "\n[MIXER]\nINSERT 1 name=\"Master\"\n";
}

} // namespace

TEST_CASE("audio_clip_plays_its_file_exactly", "[clip][audio]") {
    ClipDir const dir;
    const std::vector<float> noise = adx::tests::stereoNoise(kRate, 7);
    const fs::path file = dir.write("noise.wav", noise, kRate);
    const auto loaded = loadText(project("  AUDIO \"" + file.generic_string() + "\" 0:1:0\n"));

    const std::vector<float> out = render(loaded->project, 3 * kBeat);
    // Silence for a beat, then the file sample for sample, then silence again.
    for (std::size_t i = 0; i < kBeat * 2; ++i) {
        REQUIRE(out[i] == 0.0F);
    }
    for (std::size_t i = 0; i < noise.size() && (kBeat * 2) + i < out.size(); ++i) {
        REQUIRE(out[(kBeat * 2) + i] == noise[i]);
    }
}

TEST_CASE("audio_clip_block_size_invariant", "[clip][audio]") {
    ClipDir const dir;
    const fs::path file = dir.write("noise.wav", adx::tests::stereoNoise(kRate / 2, 3), kRate);
    const fs::path other = dir.write("other.wav", adx::tests::stereoNoise(kRate / 3, 4), 44100);
    const auto loaded = loadText(project(
        "  AUDIO \"" + file.generic_string() +
        "\" 0:0:960 offset=0:0:1920 fadein=0:0:2000 fadeout=0:1:0 fadeoutcurve=exponential(0.4)\n"
        "    ENVELOPE pan\n      0:0:0 -1\n      0:2:0 1\n"
        "  AUDIO \"" +
        other.generic_string() +
        "\" 0:0:3000 length=0:1:0 pitch=0\n"
        "    ENVELOPE pitch\n      0:0:0 0\n      0:1:0 700\n"));
    const std::vector<float> a = render(loaded->project, 2 * kBeat, 64);
    const std::vector<float> b = render(loaded->project, 2 * kBeat, 256);
    const std::vector<float> c = render(loaded->project, 2 * kBeat, 1024);
    REQUIRE(a == b);
    REQUIRE(b == c);
}

TEST_CASE("audio_clip_slip_and_trim", "[clip][audio]") {
    ClipDir const dir;
    const std::vector<float> noise = adx::tests::stereoNoise(kRate, 11);
    const fs::path file = dir.write("noise.wav", noise, kRate);
    // Slipped by a beat and trimmed to one: plays the file's second half-second only.
    const auto loaded = loadText(
        project("  AUDIO \"" + file.generic_string() + "\" 0:0:0 offset=0:1:0 length=0:1:0\n"));
    const std::vector<float> out = render(loaded->project, 2 * kBeat);
    for (std::size_t i = 0; i < kBeat * 2; ++i) {
        REQUIRE(out[i] == noise[(kBeat * 2) + i]);
    }
    for (std::size_t i = kBeat * 2; i < out.size(); ++i) {
        REQUIRE(out[i] == 0.0F);
    }
}

TEST_CASE("clip_fade_uses_engine_curve", "[clip][audio]") {
    ClipDir const dir;
    const fs::path file = dir.write("dc.wav", adx::tests::stereoConstant(kRate, 1.0F), kRate);
    const auto loaded = loadText(project("  AUDIO \"" + file.generic_string() +
                                         "\" 0:0:0 fadein=0:1:0 fadeincurve=exponential(0.5)\n"));
    const std::vector<float> out = render(loaded->project, kBeat + 100);
    adx::core::Curve const curve{.kind = adx::core::CurveKind::Exponential, .tension = 0.5F};
    for (std::size_t frame = 0; frame < kBeat; frame += 97) {
        const float expected = curve.evaluate(static_cast<float>(frame) / kBeat);
        INFO(frame);
        // The fade compiles to a line chain within kClipGainTolerance of the curve, by
        // the subdivision automation uses (phase_4.md §4.0). The error is measured at
        // each piece's quarter points, so between them it can stray a little further.
        REQUIRE_THAT(out[frame * 2],
                     Catch::Matchers::WithinAbs(expected, adx::clip::kClipGainTolerance * 1.5));
    }
    REQUIRE(out[(kBeat + 50) * 2] == 1.0F);
}

TEST_CASE("clip_fade_equals_gain_envelope", "[clip][audio]") {
    ClipDir const dir;
    const fs::path file = dir.write("noise.wav", adx::tests::stereoNoise(kRate, 5), kRate);
    const std::string path = file.generic_string();
    const auto faded =
        loadText(project("  AUDIO \"" + path + "\" 0:0:0 fadein=0:1:0 fadeincurve=smooth\n"));
    const auto enveloped = loadText(project("  AUDIO \"" + path +
                                            "\" 0:0:0\n    ENVELOPE gain\n      0:0:0 0 smooth\n"
                                            "      0:1:0 1\n"));
    REQUIRE(render(faded->project, 2 * kBeat) == render(enveloped->project, 2 * kBeat));
}

TEST_CASE("clip_crossfade_equal_power", "[clip][audio]") {
    ClipDir const dir;
    // Left-only and right-only DC, so each output side is one clip's gain.
    const fs::path left = dir.write("left.wav", adx::tests::stereoSides(kRate, 1.0F, 0.0F), kRate);
    const fs::path right =
        dir.write("right.wav", adx::tests::stereoSides(kRate, 0.0F, 1.0F), kRate);
    const auto loaded = loadText(project("  AUDIO \"" + left.generic_string() +
                                         "\" 0:0:0\n"
                                         "  AUDIO \"" +
                                         right.generic_string() + "\" 0:1:0\n"));
    const std::vector<float> out = render(loaded->project, 3 * kBeat);
    // The overlap is the second beat: [kBeat, 2 * kBeat).
    for (std::size_t frame = kBeat; frame < 2 * kBeat; frame += 37) {
        const double l = out[frame * 2];
        const double r = out[(frame * 2) + 1];
        INFO(frame);
        REQUIRE_THAT((l * l) + (r * r), Catch::Matchers::WithinAbs(1.0, 2e-3));
    }
    CHECK(out[(kBeat / 2) * 2] == 1.0F);
    CHECK(out[(((kBeat * 5) / 2) * 2) + 1] == 1.0F);

    const auto linear = loadText(project("  AUDIO \"" + left.generic_string() +
                                         "\" 0:0:0\n"
                                         "  AUDIO \"" +
                                         right.generic_string() + "\" 0:1:0 crossfade=linear\n"));
    const std::vector<float> lin = render(linear->project, 3 * kBeat);
    for (std::size_t frame = kBeat; frame < 2 * kBeat; frame += 37) {
        REQUIRE_THAT(lin[frame * 2] + lin[(frame * 2) + 1], Catch::Matchers::WithinAbs(1.0, 1e-5));
    }
}

TEST_CASE("audio_clip_rate_conversion", "[clip][audio]") {
    ClipDir const dir;
    // Half a second at 44.1 kHz plays for half a second at 48.
    const fs::path file =
        dir.write("tone.wav", adx::tests::stereoSine(44100 / 2, 44100, 1000.0), 44100);
    const auto loaded = loadText(project("  AUDIO \"" + file.generic_string() + "\" 0:0:0\n"));
    const std::vector<float> out = render(loaded->project, kRate);
    std::size_t last = 0;
    for (std::size_t frame = 0; frame < kRate; ++frame) {
        if (std::abs(out[frame * 2]) > 1e-3F) {
            last = frame;
        }
    }
    CHECK(last > (kRate / 2) - 100);
    CHECK(last < (kRate / 2) + 100);
    CHECK_THAT(adx::tests::measuredFrequency(out, kRate, 2000, 20000),
               Catch::Matchers::WithinAbs(1000.0, 0.5));
}

TEST_CASE("audio_track_routes_to_its_insert", "[clip][audio]") {
    ClipDir const dir;
    const fs::path file = dir.write("dc.wav", adx::tests::stereoConstant(kRate, 1.0F), kRate);
    const auto loaded = loadText(
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\nTRACK 1 out=insert.2\n"
        "  AUDIO \"" +
        file.generic_string() +
        "\" 0:0:0\n\n[MIXER]\nINSERT 1 name=\"Master\"\n"
        "INSERT 2 name=\"Quiet\" gain=0.25\nROUTE insert.2 -> insert.1\n");
    const std::vector<float> out = render(loaded->project, 1000);
    CHECK(out[1000] == 0.25F);
}

TEST_CASE("audio_clip_muted_track_is_silent", "[clip][audio]") {
    ClipDir const dir;
    const fs::path file = dir.write("dc.wav", adx::tests::stereoConstant(kRate, 1.0F), kRate);
    const auto loaded =
        loadText(project("  AUDIO \"" + file.generic_string() + "\" 0:0:0\n", " mute=yes"));
    const std::vector<float> out = render(loaded->project, 1000);
    for (const float sample : out) {
        REQUIRE(sample == 0.0F);
    }
}

namespace {

/// The project's only item, on its only track.
adx::project::PlaylistItem& onlyItem(adx::project::Project& p) {
    REQUIRE(p.playlist.tracks.size() == 1);
    REQUIRE(p.playlist.tracks[0].items.size() == 1);
    return p.playlist.tracks[0].items[0];
}

void setItem(adx::tests::Loaded& loaded, const adx::project::PlaylistItem& value) {
    const adx::core::PlaylistTrackId track = loaded.project.playlist.tracks[0].id;
    loaded.stack.execute(
        std::make_unique<adx::project::SetPlaylistItem>(track, value.id, value, track),
        loaded.project);
}

/// A gain ramp from 0 to 1 over two beats, then flat: easy to see where it is.
const char* const kRamp = "    ENVELOPE gain\n      0:0:0 0\n      0:2:0 1\n";

} // namespace

TEST_CASE("clip_slip_preserves_length", "[clip][audio]") {
    ClipDir const dir;
    const std::vector<float> noise =
        adx::tests::stereoNoise(static_cast<std::size_t>(kRate) * 2, 13);
    const fs::path file = dir.write("noise.wav", noise, kRate);
    auto loaded =
        loadText(project("  AUDIO \"" + file.generic_string() + "\" 0:1:0 length=0:2:0\n"));
    const adx::project::PlaylistItem before = onlyItem(loaded->project);
    adx::project::PlaylistItem slipped = before;
    slipped.sourceOffset = adx::core::Ticks{adx::core::kPpq / 2};
    setItem(*loaded, slipped);

    const adx::project::PlaylistItem& after = onlyItem(loaded->project);
    CHECK(after.start == before.start);
    CHECK(after.length == before.length);
    CHECK(after.sourceOffset == slipped.sourceOffset);
    // The same window on the arrangement, a quarter second further into the file.
    const std::vector<float> out = render(loaded->project, 4 * kBeat);
    for (std::size_t i = 0; i < kBeat * 2; ++i) {
        REQUIRE(out[i] == 0.0F);
    }
    for (std::size_t i = 0; i < kBeat * 4; ++i) {
        REQUIRE(out[(kBeat * 2) + i] == noise[(kBeat / 2 * 2) + i]);
    }
    for (std::size_t i = kBeat * 6; i < out.size(); ++i) {
        REQUIRE(out[i] == 0.0F);
    }
}

TEST_CASE("clip_envelope_travels_on_move", "[clip][audio]") {
    ClipDir const dir;
    const fs::path file = dir.write(
        "noise.wav", adx::tests::stereoNoise(static_cast<std::size_t>(kRate) * 2, 17), kRate);
    auto loaded = loadText(project("  AUDIO \"" + file.generic_string() + "\" 0:0:0\n" + kRamp));
    const std::vector<float> here = render(loaded->project, 4 * kBeat);
    adx::project::PlaylistItem moved = onlyItem(loaded->project);
    moved.start = adx::core::Ticks{adx::core::kPpq};
    setItem(*loaded, moved);
    const std::vector<float> there = render(loaded->project, 5 * kBeat);
    // A beat later, sample for sample: the envelope went with the clip.
    for (std::size_t i = 0; i < kBeat * 2; ++i) {
        REQUIRE(there[i] == 0.0F);
    }
    for (std::size_t i = 0; i < here.size(); ++i) {
        REQUIRE(there[(kBeat * 2) + i] == here[i]);
    }
}

TEST_CASE("clip_envelope_survives_slip", "[clip][audio]") {
    // Slip moves the audio under the envelope, not the envelope: the gain at each
    // instant of the item is the same, applied to later audio.
    ClipDir const dir;
    const std::vector<float> noise =
        adx::tests::stereoNoise(static_cast<std::size_t>(kRate) * 2, 19);
    const fs::path file = dir.write("noise.wav", noise, kRate);
    const fs::path ones = dir.write(
        "ones.wav", adx::tests::stereoConstant(static_cast<std::size_t>(kRate) * 2, 1.0F), kRate);
    const std::string keys = " length=0:3:0\n";
    const auto gain =
        loadText(project("  AUDIO \"" + ones.generic_string() + "\" 0:0:0" + keys + kRamp));
    const std::vector<float> curve = render(gain->project, 3 * kBeat);

    auto loaded =
        loadText(project("  AUDIO \"" + file.generic_string() + "\" 0:0:0" + keys + kRamp));
    adx::project::PlaylistItem slipped = onlyItem(loaded->project);
    slipped.sourceOffset = adx::core::Ticks{adx::core::kPpq / 2};
    setItem(*loaded, slipped);
    REQUIRE(onlyItem(loaded->project).envelopes == slipped.envelopes);
    const std::vector<float> out = render(loaded->project, 3 * kBeat);
    const std::size_t shift = kBeat; // a quarter second, in interleaved samples
    for (std::size_t i = 0; i < out.size(); ++i) {
        INFO(i);
        REQUIRE_THAT(out[i], Catch::Matchers::WithinAbs(noise[shift + i] * curve[i], 1e-6));
    }
}

TEST_CASE("clip_envelope_split_is_continuous", "[clip][audio]") {
    // Sliced inside a ramp, on a breakpoint, or between two - the two halves play what
    // the one clip did. Linear segments, so the cut is exact. (A cut inside a fade is
    // not: a fade cannot start part-way, phase_8.md §11.)
    ClipDir const dir;
    const fs::path file = dir.write(
        "noise.wav", adx::tests::stereoNoise(static_cast<std::size_t>(kRate) * 2, 23), kRate);
    const std::string text = project("  AUDIO \"" + file.generic_string() +
                                     "\" 0:0:0 length=0:3:0 fadein=0:0:960 fadeout=0:0:960\n" +
                                     kRamp + "    ENVELOPE pan\n      0:0:0 -1\n      0:3:0 1\n");
    const auto whole = loadText(text);
    const std::vector<float> reference = render(whole->project, 4 * kBeat);
    for (const std::int64_t cutAt :
         {adx::core::kPpq / 3, adx::core::kPpq * 2, adx::core::kPpq * 5 / 2}) {
        INFO("cut at " << cutAt);
        auto loaded = loadText(text);
        const adx::project::PlaylistItem item = onlyItem(loaded->project);
        const auto halves = adx::clip::splitItem(
            loaded->project, item, adx::core::Ticks{adx::core::kPpq * 3}, adx::core::Ticks{cutAt});
        REQUIRE(halves.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked by the REQUIRE above.
        const adx::clip::SplitItem& cut = *halves;
        setItem(*loaded, cut.left);
        const adx::core::PlaylistTrackId track = loaded->project.playlist.tracks[0].id;
        loaded->stack.execute(std::make_unique<adx::project::AddPlaylistItem>(track, cut.right),
                              loaded->project);
        const std::vector<float> out = render(loaded->project, 4 * kBeat);
        for (std::size_t i = 0; i < out.size(); ++i) {
            INFO(i);
            REQUIRE_THAT(out[i], Catch::Matchers::WithinAbs(reference[i], 1e-5));
        }
    }
    // Outside the clip there is nothing to cut.
    const auto loaded = loadText(text);
    CHECK_FALSE(adx::clip::splitItem(loaded->project, onlyItem(loaded->project),
                                     adx::core::Ticks{adx::core::kPpq * 3},
                                     adx::core::Ticks{adx::core::kPpq * 3}));
}

TEST_CASE("clip_split_stretched_continues_the_source", "[clip][audio]") {
    // A stretched clip's right half starts where the stretched audio had got to, not a
    // tick-for-tick offset (the slice tool's old rule).
    ClipDir const dir;
    const fs::path file = dir.write(
        "noise.wav", adx::tests::stereoNoise(static_cast<std::size_t>(kRate) * 2, 29), kRate);
    const std::string text =
        project("  AUDIO \"" + file.generic_string() + "\" 0:0:0 length=0:2:0 stretch=2\n");
    auto loaded = loadText(text);
    const adx::project::PlaylistItem item = onlyItem(loaded->project);
    const auto halves =
        adx::clip::splitItem(loaded->project, item, adx::core::Ticks{adx::core::kPpq * 2},
                             adx::core::Ticks{adx::core::kPpq});
    REQUIRE(halves.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): checked by the REQUIRE above.
    const adx::clip::SplitItem& cut = *halves;
    // One beat into a 2x stretch is half a beat into the source.
    CHECK(cut.right.sourceOffset == adx::core::Ticks{adx::core::kPpq / 2});
    CHECK(cut.left.sourceOffset == item.sourceOffset);
}

TEST_CASE("playlist_audio_peaks_arrive_unpinned", "[clip][geometry]") {
    // The playlist draws a clip's rendered buffer (ClipRenderCache), which holds entries
    // weakly. With no engine playing the clip to pin it, the drawing must pin it itself -
    // before it did, every rebuild re-requested a render that was dropped before it ran,
    // and the waveform never appeared.
    ClipDir const dir;
    const fs::path file = dir.write(
        "noise.wav", adx::tests::stereoNoise(static_cast<std::size_t>(kRate) * 2, 31), kRate);
    const auto loaded = loadText(project("  AUDIO \"" + file.generic_string() + "\" 0:0:0\n"));
    adx::geometry::PlaylistGeometry geometry;
    adx::geometry::ClipPeakCache peaks;
    const adx::geometry::PlaylistViewport view{.tickStart = 0,
                                               .tickEnd = adx::core::kPpq * 16.0,
                                               .rowStart = 0,
                                               .rowEnd = 8,
                                               .pixelsPerTick = 0.05F,
                                               .pixelsPerRow = 48};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        geometry.build(loaded->project, view,
                       adx::geometry::PlaylistBuildOptions{
                           .selected = {}, .peaks = &peaks, .renderRate = kRate});
    } while (geometry.pendingPeaks());
}
