// The C++ half of the piano roll's performance gate (phase_5.md §4.9).
//
// Pure C++, headless, no GPU. Each budget is the best of many runs, which measures the
// code rather than the shared runner's scheduling; the gate is enforced in optimised
// builds (NDEBUG) and only reported in Debug, where the numbers mean nothing.
// tests/python/test_perf_geometry.py is the other half: the FFI round trip and the
// call counts.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "engine/geometry/HitTest.h"
#include "engine/geometry/PianoRollGeometry.h"
#include "tests/cpp/geometry/GeometryFixtures.h"

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
    std::printf("bench_piano_roll: %-34s %8.4f ms (budget %.2f ms)%s\n", what, ms, budget,
                kEnforce ? "" : " [not enforced: unoptimised build]");
}

/// The 10k-note pattern the budgets are written for: 64 bars, so the whole pattern at
/// a 1600-px width is not in the density LOD and every note is a quad.
struct Bench {
    adx::test::RollScene scene{10000, 64};
    PianoRollGeometry geometry;
    BuildOptions options;
    Bench() {
        options.pattern = scene.pattern;
        options.channel = scene.lead;
        options.dataRevision = scene.stack.revision();
    }
    [[nodiscard]] Viewport fullView() const {
        const auto ticks = static_cast<float>(scene.project.find(scene.pattern)->length.value);
        // 0.0065 px/tick: a sixteenth is ~6 px, comfortably above the density LOD.
        const float ppt = 0.0065F;
        return Viewport::make(0.0, 128.0F, ppt, 8.0F, ppt * ticks, 1024.0F);
    }
};

} // namespace

TEST_CASE("bench_piano_roll_build_full", "[geometry][perf-gate]") {
    Bench bench;
    const Viewport view = bench.fullView();
    REQUIRE_FALSE(densityLod(view));
    bench.geometry.build(bench.scene.project, view, bench.options);
    REQUIRE(bench.geometry.visibleNotes().size() > 5000);
    const double ms =
        bestMs(50, [&] { bench.geometry.build(bench.scene.project, view, bench.options); });
    report("build, 10k notes, full view", ms, 2.0);
    if (kEnforce) {
        REQUIRE(ms < 2.0);
    }
}

TEST_CASE("bench_piano_roll_build_one_bar", "[geometry][perf-gate]") {
    Bench bench;
    // One bar of 4/4 across 1600 px, in the middle of the pattern, every row.
    const float ppt = 1600.0F / static_cast<float>(4 * kPpq);
    const Viewport view = Viewport::make(32.0 * 4 * kPpq, 128.0F, ppt, 8.0F, 1600.0F, 1024.0F);
    bench.geometry.build(bench.scene.project, view, bench.options);
    const std::size_t visible = bench.geometry.visibleNotes().size();
    REQUIRE(visible > 50);
    REQUIRE(visible < 1000);
    const double ms =
        bestMs(200, [&] { bench.geometry.build(bench.scene.project, view, bench.options); });
    report("build, zoomed to one bar", ms, 0.10);
    if (kEnforce) {
        REQUIRE(ms < 0.10);
    }
}

TEST_CASE("bench_piano_roll_hit_rect", "[geometry][perf-gate]") {
    Bench bench;
    const Viewport view = bench.fullView();
    std::vector<adx::core::NoteId> out(10000);
    std::size_t count = 0;
    const double ms = bestMs(100, [&] {
        count = hitTestRect(bench.scene.project, bench.scene.pattern, bench.scene.lead, view,
                            Rect{.x0 = 0.0F, .y0 = 0.0F, .x1 = view.widthPx, .y1 = 1024.0F}, out);
    });
    REQUIRE(count == 10000);
    report("hitTestRect over 10k notes", ms, 0.20);
    if (kEnforce) {
        REQUIRE(ms < 0.20);
    }
}

TEST_CASE("bench_piano_roll_steady_state_storage", "[geometry][perf-gate]") {
    // The allocation count itself is geometry_no_alloc_steady_state, under the
    // allocator hook - which Release compiles out. Here, in every build, the proxy: no
    // buffer's storage moves across 1000 rebuilds after warm-up.
    Bench bench;
    const Viewport view = bench.fullView();
    bench.geometry.build(bench.scene.project, view, bench.options);
    std::vector<const float*> before;
    before.reserve(6);
    for (std::size_t b = 0; b < 6; ++b) {
        before.push_back(bench.geometry.buffer(b).data());
    }
    for (int i = 0; i < 1000; ++i) {
        bench.geometry.build(bench.scene.project, view, bench.options);
    }
    for (std::size_t b = 0; b < 6; ++b) {
        REQUIRE(bench.geometry.buffer(b).data() == before[b]);
    }
}
