// The zero-copy contract and the piano-roll builder (phase_5.md §4.1, §4.4, §5).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/GeometryBuffer.h"
#include "engine/geometry/PianoRollGeometry.h"
#include "engine/rt/AllocGuard.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/geometry/GeometryFixtures.h"

using adx::core::kPpq;
using adx::core::NoteId;
using namespace adx::geometry;

namespace {

Viewport fullView(const adx::test::RollScene& scene, float widthPx = 1600.0F) {
    const auto ticks = static_cast<double>(scene.project.find(scene.pattern)->length.value);
    return Viewport::make(0.0, 128.0F, static_cast<float>(widthPx / ticks), 8.0F, widthPx, 1024.0F);
}

BuildOptions optionsFor(const adx::test::RollScene& scene) {
    BuildOptions options;
    options.pattern = scene.pattern;
    options.channel = scene.lead;
    options.dataRevision = scene.stack.revision();
    return options;
}

/// Every note a viewport can show, by the definition the builder documents.
std::set<std::uint32_t> bruteVisible(const adx::test::RollScene& scene, const Viewport& view) {
    std::set<std::uint32_t> ids;
    for (const auto& note : scene.clip().notes) {
        const auto p = static_cast<float>(note.pitch);
        if (static_cast<double>(note.end().value) < view.tickStart ||
            static_cast<double>(note.start.value) > view.tickEnd || p + 1.0F < view.pitchLow ||
            p > view.pitchHigh) {
            continue;
        }
        ids.insert(note.id.value);
    }
    return ids;
}

} // namespace

TEST_CASE("geometry_buffer_revision", "[geometry]") {
    GeometryBuffer buffer;
    const auto r0 = buffer.revision();
    auto out = buffer.resize(4, kFloatsPerVertex);
    out[0] = 1.0F;
    const auto r1 = buffer.revision();
    REQUIRE(r1 > r0);
    // Reads never bump it.
    REQUIRE(buffer.data()[0] == 1.0F);
    REQUIRE(buffer.vertexCount() == 4);
    REQUIRE(buffer.floats().size() == 12);
    REQUIRE(buffer.revision() == r1);
    static_cast<void>(buffer.patch());
    REQUIRE(buffer.revision() > r1);
}

TEST_CASE("geometry_lease_prevents_resize", "[geometry]") {
    GeometryBuffer buffer;
    static_cast<void>(buffer.resize(16, kFloatsPerVertex));
    const float* before = buffer.data();
    {
        GeometryLease lease(buffer);
        REQUIRE_THROWS_AS(buffer.resize(1 << 20, kFloatsPerVertex), std::logic_error);
        REQUIRE_THROWS_AS(buffer.patch(), std::logic_error);
        REQUIRE(buffer.data() == before);
        REQUIRE(lease.release());
        REQUIRE_NOTHROW(buffer.resize(32, kFloatsPerVertex));
    }
    // A lease that sees the revision move reports it.
    GeometryLease lease(buffer);
    buffer.releaseLease();
    static_cast<void>(buffer.resize(8, kFloatsPerVertex));
    buffer.acquireLease();
    REQUIRE_FALSE(lease.release());
}

TEST_CASE("geometry_no_alloc_steady_state", "[geometry]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SKIP("the allocator hook is compiled out of Release builds");
    }
    const adx::test::RollScene scene(10000);
    PianoRollGeometry geometry;
    const BuildOptions options = optionsFor(scene);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> start(0.0, 200.0 * 4 * kPpq);
    std::vector<Viewport> views;
    views.reserve(17);
    for (int i = 0; i < 16; ++i) {
        views.push_back(Viewport::make(start(rng), 96.0F, 0.02F, 12.0F, 1600.0F, 800.0F));
    }
    views.push_back(fullView(scene));
    // Warm-up: every buffer grows to the largest of these views once.
    for (const Viewport& view : views) {
        geometry.build(scene.project, view, options);
    }
    auto& log = adx::rt::ViolationLog::instance();
    log.reset();
    {
        const adx::rt::ScopedRtSection section;
        for (int i = 0; i < 1000; ++i) {
            geometry.build(scene.project, views[static_cast<std::size_t>(i) % views.size()],
                           options);
        }
    }
    REQUIRE(log.count(adx::rt::ViolationKind::Allocation) == 0);
}

TEST_CASE("geometry_culling_correct", "[geometry]") {
    const adx::test::RollScene scene(10000);
    PianoRollGeometry geometry;
    const BuildOptions options = optionsFor(scene);
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> start(-1000.0, 256.0 * 4 * kPpq);
    std::uniform_real_distribution<float> top(40.0F, 128.0F);
    std::uniform_real_distribution<float> zoom(0.01F, 0.3F);
    for (int i = 0; i < 20; ++i) {
        const Viewport view =
            Viewport::make(start(rng), top(rng), zoom(rng), 10.0F, 1200.0F, 500.0F);
        geometry.build(scene.project, view, options);
        std::set<std::uint32_t> built;
        for (const NoteId id : geometry.visibleNotes()) {
            built.insert(id.value);
        }
        REQUIRE(built == bruteVisible(scene, view));
        REQUIRE(geometry.notes().vertexCount() == built.size() * 6);
        REQUIRE(geometry.lanes().vertexCount() == built.size() * 6);
    }
}

TEST_CASE("geometry_lod_threshold", "[geometry]") {
    const adx::test::RollScene scene(10000);
    PianoRollGeometry geometry;
    const BuildOptions options = optionsFor(scene);
    const float threshold = kDensityPixelsPerSixteenth / (static_cast<float>(kPpq) / 4.0F);
    const auto ticks = static_cast<float>(scene.project.find(scene.pattern)->length.value);

    // Two zooms either side of the boundary, each viewing the whole pattern.
    const Viewport fine =
        Viewport::make(0.0, 128.0F, threshold * 1.01F, 8.0F, threshold * 1.01F * ticks, 1024.0F);
    const Viewport coarse =
        Viewport::make(0.0, 128.0F, threshold * 0.99F, 8.0F, threshold * 0.99F * ticks, 1024.0F);
    REQUIRE_FALSE(densityLod(fine));
    REQUIRE(densityLod(coarse));

    geometry.build(scene.project, fine, options);
    const std::size_t fineVertices = geometry.notes().vertexCount();
    const std::size_t rows = geometry.rows().vertexCount();
    const std::size_t ghosts = geometry.ghosts().vertexCount();
    // The extent the notes cover, as a set of pitch rows.
    const auto rowsCovered = [&geometry] {
        std::set<float> covered;
        const auto floats = geometry.notes().floats();
        for (std::size_t v = 0; v < floats.size(); v += kFloatsPerVertex * 6) {
            covered.insert(std::floor(floats[v + 1]));
        }
        return covered;
    };
    const auto fineRows = rowsCovered();

    geometry.build(scene.project, coarse, options);
    REQUIRE(geometry.density());
    REQUIRE(geometry.notes().vertexCount() < fineVertices);
    // Nothing else changes: the same rows are covered, the background and ghosts are
    // untouched by the note LOD.
    REQUIRE(rowsCovered() == fineRows);
    REQUIRE(geometry.rows().vertexCount() == rows);
    REQUIRE(geometry.ghosts().vertexCount() == ghosts);
}

TEST_CASE("geometry_selection_patches_in_place", "[geometry]") {
    const adx::test::RollScene scene(500, 16);
    PianoRollGeometry geometry;
    const BuildOptions options = optionsFor(scene);
    geometry.build(scene.project, fullView(scene), options);
    const std::vector<float> before(geometry.notes().floats().begin(),
                                    geometry.notes().floats().end());
    const auto revision = geometry.notes().revision();
    const float* storage = geometry.notes().data();

    std::vector<NoteId> selected(geometry.visibleNotes().begin(),
                                 geometry.visibleNotes().begin() + 50);
    std::ranges::sort(selected);
    geometry.updateSelection(selected);

    const auto after = geometry.notes().floats();
    REQUIRE(geometry.notes().revision() > revision);
    REQUIRE(geometry.notes().data() == storage);
    std::size_t recoloured = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (i % kFloatsPerVertex != 2) {
            REQUIRE(after[i] == before[i]); // no vertex moved
        } else if (after[i] != before[i]) {
            REQUIRE(after[i] == roleValue(ColorRole::kNoteSelected));
            ++recoloured;
        }
    }
    REQUIRE(recoloured == std::size_t{50} * 6);
}

TEST_CASE("geometry_scale_highlight_rows", "[geometry]") {
    const adx::test::RollScene scene(10, 4);
    PianoRollGeometry geometry;
    BuildOptions options = optionsFor(scene);
    options.scale = ScaleHighlight{.root = 2, .mask = 0b1010'1011'0101}; // D major
    geometry.build(scene.project, Viewport::make(0.0, 72.0F, 0.05F, 10.0F, 800.0F, 120.0F),
                   options);
    // 12 rows, 60..71: D (62) is the root, C# (61) in the scale, C (60) not.
    const auto floats = geometry.rows().floats();
    REQUIRE(floats.size() == std::size_t{12} * 6 * kFloatsPerVertex);
    const auto roleOfRow = [&floats](int pitch) {
        for (std::size_t v = 0; v < floats.size(); v += kFloatsPerVertex) {
            if (floats[v + 1] == worldRowTop(pitch)) {
                return floats[v + 2];
            }
        }
        return -1.0F;
    };
    REQUIRE(roleOfRow(62) == roleValue(ColorRole::kRowRoot));
    REQUIRE(roleOfRow(61) == roleValue(ColorRole::kRowInScale));
    REQUIRE(roleOfRow(60) == roleValue(ColorRole::kRowBlack));
    REQUIRE(geometry.grid().vertexCount() > 0);
}
