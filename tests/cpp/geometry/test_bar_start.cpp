// TempoMap::barStart and the piano roll's bar lines read one implementation
// (phase_11.6.md §2), so the Cut tool cuts where the grid draws the bar.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <set>

#include "engine/core/TempoMap.h"
#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/PianoRollGeometry.h"
#include "tests/cpp/geometry/GeometryFixtures.h"

using adx::core::kPpq;
using adx::core::Ticks;
using namespace adx::geometry;

TEST_CASE("bar_start_within_a_meter", "[tempomap]") {
    adx::core::TempoMap map;
    const std::int64_t bar = 4 * kPpq;
    REQUIRE(map.barStart(Ticks{0}).value == 0);
    REQUIRE(map.barStart(Ticks{bar - 1}).value == 0);
    REQUIRE(map.barStart(Ticks{bar}).value == bar);
    REQUIRE(map.barStart(Ticks{(3 * bar) + 17}).value == 3 * bar);
    REQUIRE(map.barStart(Ticks{-1}).value == -bar);

    // 3/4 from the second beat of bar 2: bars restart at the change.
    const std::int64_t change = (2 * bar) + kPpq;
    map.setMeter(Ticks{change}, 3, 4);
    REQUIRE(map.barStart(Ticks{change - 1}).value == 2 * bar);
    REQUIRE(map.barStart(Ticks{change}).value == change);
    REQUIRE(map.barStart(Ticks{change + (3 * kPpq)}).value == change + (3 * kPpq));
    REQUIRE(map.barStart(Ticks{change + (3 * kPpq) - 1}).value == change);
}

TEST_CASE("test_bar_start_matches_grid_lines", "[geometry][tempomap]") {
    adx::test::RollScene scene(10, 8);
    auto& tempo = scene.project.tempo;
    const std::int64_t bar = 4 * kPpq;
    tempo.setMeter(Ticks{(2 * bar) + kPpq}, 3, 4); // mid-bar change
    tempo.setMeter(Ticks{(2 * bar) + kPpq + (6 * kPpq)}, 7, 8);
    const auto length = static_cast<double>(scene.project.find(scene.pattern)->length.value);

    PianoRollGeometry geometry;
    BuildOptions options;
    options.pattern = scene.pattern;
    options.channel = scene.lead;
    options.dataRevision = scene.stack.revision();
    const float ppt = 0.01F; // fine enough that every eighth note gets a line
    geometry.build(
        scene.project,
        Viewport::make(0.0, 128.0F, ppt, 8.0F, static_cast<float>(length * ppt), 1024.0F), options);

    std::set<std::int64_t> drawn;
    const auto floats = geometry.grid().floats();
    REQUIRE_FALSE(floats.empty());
    for (std::size_t v = 0; v < floats.size(); v += 2 * kFloatsPerVertex) {
        if (floats[v + 2] != roleValue(ColorRole::kGridBar)) {
            continue;
        }
        drawn.insert(std::llround(static_cast<double>(floats[v]) * kPpq));
    }

    std::set<std::int64_t> expected;
    const auto end = static_cast<std::int64_t>(length);
    for (std::int64_t t = 0; t < end - kPpq; t += kPpq / 2) {
        expected.insert(tempo.barStart(Ticks{t}).value);
    }
    for (const std::int64_t t : drawn) {
        INFO("bar line at " << t);
        REQUIRE(tempo.barStart(Ticks{t}).value == t);
    }
    for (const std::int64_t t : expected) {
        INFO("bar start " << t);
        REQUIRE(drawn.contains(t));
    }
    REQUIRE(drawn.contains((2 * bar) + kPpq));
}
