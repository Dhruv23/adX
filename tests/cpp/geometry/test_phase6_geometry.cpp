// Phase 6's geometry builders: correctness, and the extended perf_geometry_budgets
// (phase_6.md §3 rule 6, §5) - playlist, step grid, automation, meter and EQ curve each
// within budget. Budgets are enforced in optimised builds and reported in Debug.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "engine/dsp/RbjFilter.h"
#include "engine/format/adx/Parser.h"
#include "engine/geometry/AutomationGeometry.h"
#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/EqCurveGeometry.h"
#include "engine/geometry/MeterGeometry.h"
#include "engine/geometry/PlaylistGeometry.h"
#include "engine/geometry/StepGridGeometry.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"

using adx::core::kPpq;
using namespace adx::geometry;

namespace {

#ifdef NDEBUG
constexpr bool kEnforce = true;
#else
constexpr bool kEnforce = false;
#endif

template<class Body> double bestMs(int runs, Body&& body) {
    double best = 1e30;
    for (int i = 0; i < runs; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        body();
        const auto end = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(end - begin).count());
    }
    return best;
}

void report(const char* what, double ms, double budget) {
    std::printf("perf_geometry_budgets: %-40s %8.4f ms (budget %.2f ms)%s\n", what, ms, budget,
                kEnforce ? "" : " [not enforced: unoptimised build]");
}

/// A project of `tracks` tracks, each holding `clips` placements of a 64-note pattern,
/// one bar apart, and `channels` channels.
struct Arrangement {
    adx::project::Project project;
    adx::project::CommandStack stack;

    Arrangement(int tracks, int clips, int channels = 4) {
        std::string text = "[PROJECT]\nADX_VERSION=2\n\n";
        for (int c = 0; c < channels; ++c) {
            text += "[CHANNEL C" + std::to_string(c) + "]\nOUTPUT=insert.1\n\n";
        }
        text += "[PATTERN P]\nLENGTH=1:0:0\n";
        for (int c = 0; c < channels; ++c) {
            text += "NOTES C" + std::to_string(c) + "\n";
            for (int n = 0; n < 16; ++n) {
                text += "  " + std::to_string(48 + ((n * 5 + c * 3) % 24)) +
                        " 0:" + std::to_string(n / 4) + ":" + std::to_string((n % 4) * 960) +
                        " 0:0:480 " + std::to_string(40 + (n * 5)) + "\n";
            }
        }
        text += "\n[PLAYLIST]\n";
        for (int t = 0; t < tracks; ++t) {
            text += "TRACK " + std::to_string(t + 1) + "\n";
            for (int i = 0; i < clips; ++i) {
                text += "  PATTERN P " + std::to_string(i) + ":0:0\n";
            }
        }
        text += "\n[MIXER]\nINSERT 1 name=\"Master\"\n";
        adx::format::DiagnosticList diagnostics;
        adx::format::load(text, project, stack, diagnostics);
        REQUIRE_FALSE(diagnostics.hasErrors());
    }
};

std::size_t countRole(const GeometryBuffer& buffer, ColorRole role) {
    std::size_t count = 0;
    const auto floats = buffer.floats();
    for (std::size_t v = 0; v < buffer.vertexCount(); ++v) {
        count += floats[(v * kFloatsPerVertex) + 2] == roleValue(role) ? 1U : 0U;
    }
    return count;
}

} // namespace

TEST_CASE("playlist_lod_switch", "[geometry][phase6]") {
    // Below the width threshold a clip is one flat rectangle: no header, no preview
    // (phase_6.md §4.1, §5).
    Arrangement const scene(4, 16);
    PlaylistGeometry geometry;
    const PlaylistViewport near{.tickStart = 0,
                                .tickEnd = kPpq * 4.0 * 16,
                                .rowStart = 0,
                                .rowEnd = 8,
                                .pixelsPerTick = 0.02F,
                                .pixelsPerRow = 48};
    geometry.build(scene.project, near, {});
    CHECK(geometry.visible().size() == 64);
    CHECK(geometry.detailed() == 64);
    CHECK(geometry.previews().vertexCount() > 0);
    CHECK(countRole(geometry.clips(), ColorRole::kClipHeader) == std::size_t{64} * 6);

    PlaylistViewport far = near;
    far.pixelsPerTick = 25.0F / (kPpq * 4.0F); // a bar is 25 px: under the threshold
    geometry.build(scene.project, far, {});
    CHECK(geometry.visible().size() == 64);
    CHECK(geometry.detailed() == 0);
    CHECK(geometry.previews().vertexCount() == 0);
    CHECK(countRole(geometry.clips(), ColorRole::kClipHeader) == 0);
    CHECK(geometry.clips().vertexCount() == std::size_t{64} * 6);
}

TEST_CASE("playlist_hit_test_finds_clip_and_edges", "[geometry][phase6]") {
    Arrangement scene(2, 4);
    PlaylistGeometry geometry;
    const PlaylistViewport view{.tickStart = 0,
                                .tickEnd = kPpq * 16.0,
                                .rowStart = 0,
                                .rowEnd = 4,
                                .pixelsPerTick = 0.025F,
                                .pixelsPerRow = 40};
    geometry.build(scene.project, view, {});
    const float bar = kPpq * 4.0F * view.pixelsPerTick; // 384 px
    const ClipHit body = geometry.hitTest(view, bar * 1.5F, 60.0F);
    REQUIRE(body.item.valid());
    CHECK(body.track == scene.project.playlist.tracks[1].id);
    CHECK(body.part == ClipPart::Body);
    CHECK(geometry.hitTest(view, (bar * 2.0F) - 2.0F, 20.0F).part == ClipPart::RightEdge);
    CHECK(geometry.hitTest(view, bar + 2.0F, 20.0F).part == ClipPart::LeftEdge);
    CHECK(geometry.hitTest(view, bar * 1.5F, 2.0F).part == ClipPart::Header);
    const ClipHit empty = geometry.hitTest(view, bar * 10.0F, 20.0F);
    CHECK_FALSE(empty.item.valid());
    CHECK(empty.track == scene.project.playlist.tracks[0].id);
}

TEST_CASE("playlist_collapsed_group_hides_members", "[geometry][phase6]") {
    Arrangement scene(3, 1);
    auto& tracks = scene.project.playlist.tracks;
    tracks[1].group = tracks[0].id;
    tracks[2].group = tracks[0].id;
    tracks[0].collapsed = true;
    std::vector<TrackRow> rows;
    layoutTracks(scene.project.playlist, rows);
    CHECK(rows[0].height == 1.0F);
    CHECK(rows[1].height == 0.0F);
    CHECK(rows[2].height == 0.0F);
}

TEST_CASE("automation_uses_engine_curve", "[geometry][phase6]") {
    // The UI's tessellation is Curve::evaluate: compared at 1000 points, within 1e-6
    // (phase_6.md §5; FINAL_PLAN §3.2).
    using adx::core::Curve;
    using adx::core::CurveKind;
    for (const CurveKind kind :
         {CurveKind::Exponential, CurveKind::Logarithmic, CurveKind::Smooth, CurveKind::Bezier}) {
        const std::vector<LanePoint> points{
            LanePoint{.tick = 0, .value = 0.2F, .curve = Curve{.kind = kind, .tension = 0.4F}},
            LanePoint{.tick = kPpq * 4, .value = 0.9F, .curve = {}}};
        const LaneRange range{.minimum = 0.0F, .maximum = 1.0F, .logarithmic = false};
        const LaneView view{.tickStart = 0,
                            .tickEnd = kPpq * 4.0,
                            .pixelsPerTick = 0.05F,
                            .heightPx = 100,
                            .segments = 1000};
        AutomationGeometry geometry;
        geometry.build(points, range, view, {});
        const auto floats = geometry.line().floats();
        REQUIRE(geometry.line().vertexCount() == 2000);
        for (std::size_t v = 0; v < geometry.line().vertexCount(); ++v) {
            const double x = floats[v * kFloatsPerVertex];
            const float y = floats[(v * kFloatsPerVertex) + 1];
            const auto t = static_cast<float>(x * kPpq / (kPpq * 4.0));
            const float expected = 1.0F - adx::core::interpolate(points[0].curve, 0.2F, 0.9F, t);
            INFO("kind " << static_cast<int>(kind) << " vertex " << v);
            REQUIRE(std::abs(y - expected) < 1e-6F);
        }
    }
}

TEST_CASE("step_grid_reads_notes_at_step_resolution", "[geometry][phase6]") {
    Arrangement scene(1, 1);
    std::vector<StepCell> cells;
    const adx::project::Pattern& pattern = scene.project.patterns.front();
    stepsOf(scene.project, pattern.id, scene.project.channels[0].id, kPpq / 4, 16, cells);
    REQUIRE(cells.size() == 16);
    // Every sixteenth holds a note in the fixture.
    CHECK(std::ranges::all_of(cells, [](const StepCell& cell) { return cell.note.valid(); }));
    CHECK(cells[0].velocity == 40);
    CHECK(cells[3].velocity == 55);
}

TEST_CASE("eq_curve_is_the_filter", "[geometry][phase6]") {
    // The drawn response is the biquads' own magnitude: a +6 dB peak at 1 kHz reads
    // +6 dB there and ~0 dB two decades away.
    const std::vector<EqBand> bands{EqBand{
        .kind = adx::dsp::BiquadKind::Peaking, .frequency = 1000.0, .q = 1.0, .gainDb = 6.0}};
    EqCurveGeometry geometry;
    geometry.build(bands, 48000.0, 24.0F, 512);
    const auto response = geometry.responseDb();
    REQUIRE(response.size() == 512);
    const auto at = [&](double hz) {
        const auto index = static_cast<std::size_t>(std::lround(eqAxisPosition(hz) * 511.0));
        return response[index];
    };
    CHECK(std::abs(at(1000.0) - 6.0) < 0.2);
    CHECK(std::abs(at(20.0)) < 0.1);
    CHECK(geometry.handles().vertexCount() == 6);

    std::vector<float> tone(4096);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * 1000.0 * static_cast<double>(i) / 48000.0));
    }
    geometry.buildSpectrum(tone, 48000.0);
    CHECK(geometry.spectrum().vertexCount() > 100);
}

TEST_CASE("meter_geometry_is_one_buffer_with_latching_clips", "[geometry][phase6]") {
    std::vector<adx::rt::LevelFrame> strips(200);
    strips[3].peakLeft = 1.2F;
    strips[4].rmsLeft = 0.5F;
    strips[4].peakLeft = 0.7F;
    MeterGeometry geometry;
    geometry.build(strips);
    const std::size_t vertices = geometry.bars().vertexCount();
    CHECK(vertices == std::size_t{200} * 8U * 6U);
    CHECK(geometry.clipped(3));
    CHECK_FALSE(geometry.clipped(4));
    strips[3].peakLeft = 0.1F;
    geometry.build(strips);
    CHECK(geometry.clipped(3)); // latched
    CHECK(geometry.bars().vertexCount() == vertices);
    geometry.resetClips();
    geometry.build(strips);
    CHECK_FALSE(geometry.clipped(3));
    CHECK(meterPosition(1.0F) == 0.0F);
    CHECK(meterPosition(0.0F) == 1.0F);
}

TEST_CASE("perf_geometry_budgets_phase6", "[geometry][perf-gate][phase6]") {
    {
        // A dense arrangement: 32 tracks x 64 one-bar clips, 2,048 clips, a view of 64
        // bars by 16 tracks with every preview drawn.
        Arrangement scene(32, 64);
        PlaylistGeometry geometry;
        const PlaylistViewport view{.tickStart = 0,
                                    .tickEnd = kPpq * 4.0 * 64,
                                    .rowStart = 0,
                                    .rowEnd = 16,
                                    .pixelsPerTick = 2000.0F / (kPpq * 4.0F * 64.0F),
                                    .pixelsPerRow = 48};
        geometry.build(scene.project, view, {});
        REQUIRE(geometry.detailed() > 0);
        const double ms = bestMs(30, [&] { geometry.build(scene.project, view, {}); });
        report("playlist build, 1,024 visible clips", ms, 2.0);
        if (kEnforce) {
            REQUIRE(ms < 2.0);
        }
    }
    {
        Arrangement scene(1, 1, 64);
        std::vector<adx::core::ChannelId> channels;
        channels.reserve(scene.project.channels.size());
        for (const auto& channel : scene.project.channels) {
            channels.push_back(channel.id);
        }
        StepGridGeometry geometry;
        const StepGridSpec spec{.pattern = scene.project.patterns.front().id,
                                .channels = channels,
                                .stepTicks = kPpq / 16,
                                .steps = 64,
                                .groupSize = 4};
        geometry.build(scene.project, spec);
        const double ms = bestMs(30, [&] { geometry.build(scene.project, spec); });
        report("step grid build, 64 x 64", ms, 0.5);
        if (kEnforce) {
            REQUIRE(ms < 0.5);
        }
    }
    {
        std::vector<LanePoint> points;
        points.reserve(512);
        for (int i = 0; i < 512; ++i) {
            points.push_back(
                LanePoint{.tick = i * (kPpq / 2),
                          .value = static_cast<float>(i % 7) / 7.0F,
                          .curve = adx::core::Curve{.kind = adx::core::CurveKind::Smooth}});
        }
        AutomationGeometry geometry;
        const LaneView view{.tickStart = 0,
                            .tickEnd = kPpq * 256.0,
                            .pixelsPerTick = 2000.0F / (kPpq * 256.0F),
                            .heightPx = 120};
        const double ms = bestMs(30, [&] { geometry.build(points, {}, view, {}); });
        report("automation lane, 512 curved points", ms, 0.5);
        if (kEnforce) {
            REQUIRE(ms < 0.5);
        }
    }
    {
        std::vector<adx::rt::LevelFrame> strips(200);
        MeterGeometry geometry;
        geometry.build(strips);
        const double ms = bestMs(100, [&] { geometry.build(strips); });
        report("meters, 200 strips", ms, 0.1);
        if (kEnforce) {
            REQUIRE(ms < 0.1);
        }
    }
    {
        std::vector<EqBand> bands(8);
        for (std::size_t b = 0; b < bands.size(); ++b) {
            bands[b].frequency = 60.0 * std::pow(2.0, static_cast<double>(b));
            bands[b].gainDb = 3.0;
        }
        EqCurveGeometry geometry;
        const double ms = bestMs(30, [&] { geometry.build(bands, 48000.0, 24.0F, 512); });
        report("EQ curve, 8 bands x 512 points", ms, 1.0);
        if (kEnforce) {
            REQUIRE(ms < 1.0);
        }
    }
}
