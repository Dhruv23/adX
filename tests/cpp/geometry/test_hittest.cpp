// Hit testing against a brute-force reference (phase_5.md §4.6, §5).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "engine/geometry/HitTest.h"
#include "tests/cpp/geometry/GeometryFixtures.h"

using adx::core::NoteId;
using namespace adx::geometry;

namespace {

/// The reference: the last stored note whose row and span contain the point.
std::uint32_t bruteNote(const adx::test::RollScene& scene, const Viewport& view, float x, float y) {
    const double tick = view.tickStart + static_cast<double>(x / view.pixelsPerTick);
    const auto row = static_cast<int>(std::floor(view.pitchHigh - (y / view.pixelsPerSemitone)));
    std::uint32_t found = 0;
    for (const auto& note : scene.clip().notes) {
        if (std::cmp_equal(note.pitch, row) && tick >= static_cast<double>(note.start.value) &&
            tick < static_cast<double>(note.end().value)) {
            found = note.id.value;
        }
    }
    return found;
}

std::set<std::uint32_t> bruteRect(const adx::test::RollScene& scene, const Viewport& view, Rect r) {
    const double t0 = view.tickAt(std::min(r.x0, r.x1));
    const double t1 = view.tickAt(std::max(r.x0, r.x1));
    const float high = view.pitchAt(std::min(r.y0, r.y1));
    const float low = view.pitchAt(std::max(r.y0, r.y1));
    std::set<std::uint32_t> ids;
    for (const auto& note : scene.clip().notes) {
        const auto p = static_cast<float>(note.pitch);
        const bool rows = p + 1.0F > low && p < high;
        const bool span = static_cast<double>(note.end().value) > t0 &&
                          static_cast<double>(note.start.value) <= t1;
        if (rows && span) {
            ids.insert(note.id.value);
        }
    }
    return ids;
}

} // namespace

TEST_CASE("hit_test_matches_bruteforce", "[geometry]") {
    const adx::test::RollScene scene(10000, 64);
    const Viewport view =
        Viewport::make(4.0 * adx::core::kPpq, 100.0F, 0.08F, 9.0F, 1500.0F, 600.0F);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> x(0.0F, view.widthPx);
    std::uniform_real_distribution<float> y(0.0F, view.heightPx);

    std::size_t hits = 0;
    for (int i = 0; i < 10000; ++i) {
        const float px = x(rng);
        const float py = y(rng);
        const NoteHit hit = hitTestNote(scene.project, scene.pattern, scene.lead, view, px, py);
        REQUIRE(hit.note.value == bruteNote(scene, view, px, py));
        REQUIRE((hit.part == NotePart::kNone) == !hit.note.valid());
        hits += hit.note.valid() ? 1 : 0;
    }
    REQUIRE(hits > 100); // the points did land on notes

    std::vector<NoteId> out(20000);
    for (int i = 0; i < 1000; ++i) {
        const Rect rect{.x0 = x(rng), .y0 = y(rng), .x1 = x(rng), .y1 = y(rng)};
        const std::size_t count =
            hitTestRect(scene.project, scene.pattern, scene.lead, view, rect, out);
        std::set<std::uint32_t> got;
        for (std::size_t k = 0; k < count; ++k) {
            got.insert(out[k].value);
        }
        REQUIRE(got == bruteRect(scene, view, rect));
    }
}

TEST_CASE("hit_test_rect_reports_overflow", "[geometry]") {
    const adx::test::RollScene scene(1000, 8);
    const Viewport view = Viewport::make(0.0, 128.0F, 0.01F, 4.0F, 2000.0F, 512.0F);
    std::vector<NoteId> small(10);
    const std::size_t count = hitTestRect(scene.project, scene.pattern, scene.lead, view,
                                          Rect{.x0 = 0, .y0 = 0, .x1 = 2000, .y1 = 512}, small);
    REQUIRE(count == 1000);
}

TEST_CASE("hit_test_edges_and_lane", "[geometry]") {
    adx::test::RollScene scene(0, 4);
    std::vector<adx::project::Note> notes(1);
    notes[0].start = adx::core::Ticks{adx::core::kPpq};
    notes[0].length = adx::core::Ticks{adx::core::kPpq};
    notes[0].pitch = 60;
    notes[0].velocity = 64;
    scene.run(std::make_unique<adx::project::AddNotes>(scene.pattern, scene.lead, notes));
    // 0.1 px per tick: the note spans x = 384 .. 768; row 60 spans y = 100 .. 110.
    const Viewport view = Viewport::make(0.0, 71.0F, 0.1F, 10.0F, 1000.0F, 200.0F);
    REQUIRE(hitTestNote(scene.project, scene.pattern, scene.lead, view, 500.0F, 105.0F).part ==
            NotePart::kBody);
    REQUIRE(hitTestNote(scene.project, scene.pattern, scene.lead, view, 765.0F, 105.0F).part ==
            NotePart::kRightEdge);
    REQUIRE(hitTestNote(scene.project, scene.pattern, scene.lead, view, 386.0F, 105.0F).part ==
            NotePart::kLeftEdge);
    REQUIRE_FALSE(
        hitTestNote(scene.project, scene.pattern, scene.lead, view, 500.0F, 115.0F).note.valid());

    const LaneHit lane =
        hitTestLane(scene.project, scene.pattern, scene.lead, view, 100.0F, 386.0F, 25.0F);
    REQUIRE(lane.note.valid());
    REQUIRE(lane.value == 0.75F);
    REQUIRE_FALSE(hitTestLane(scene.project, scene.pattern, scene.lead, view, 100.0F, 600.0F, 25.0F)
                      .note.valid());
}
