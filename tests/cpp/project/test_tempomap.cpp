// The tempo map, and the invariant Phase 3's scheduling is built on.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>

#include "engine/core/TempoMap.h"

using adx::core::BarBeatTick;
using adx::core::kPpq;
using adx::core::TempoMap;
using adx::core::Ticks;

namespace {

constexpr std::uint32_t kRate = 48000;

/// Twenty tempo changes, alternating constant and ramping, over ten minutes of
/// music. The shape the round-trip invariant has to survive.
TempoMap twentyChangeMap() {
    TempoMap map;
    std::mt19937 rng(20260913U);
    std::uniform_real_distribution<double> bpm(60.0, 200.0);
    for (int i = 0; i < 20; ++i) {
        map.setTempo(Ticks{static_cast<std::int64_t>(i) * kPpq * 16}, bpm(rng), (i % 2) == 0);
    }
    return map;
}

} // namespace

TEST_CASE("time_tick_roundtrip", "[tempomap]") {
    const TempoMap map = twentyChangeMap();

    // Segment boundaries first: they are where a closed form and a numeric
    // integration disagree most, and where an off-by-one segment lookup hides.
    for (const auto& event : map.tempoEvents()) {
        INFO("boundary at tick " << event.at.value);
        CHECK(map.toTicks(map.toSamples(event.at, kRate), kRate) == event.at);
    }

    std::mt19937 rng(1234U);
    std::uniform_int_distribution<std::int64_t> ticks(0, kPpq * 16 * 21);
    for (int i = 0; i < 100000; ++i) {
        const Ticks at{ticks(rng)};
        const Ticks back = map.toTicks(map.toSamples(at, kRate), kRate);
        if (back != at) {
            INFO("tick " << at.value << " came back as " << back.value);
            FAIL();
        }
    }
}

TEST_CASE("tempomap_ramp_closed_form", "[tempomap]") {
    // A ten-minute linear ramp from 60 to 180 bpm. Integrated in closed form:
    //   t = 60 / (PPQ k) * ln(b1 / b0),  k = (b1 - b0) / L
    // A numeric integration would drift here, and the drift is audible.
    TempoMap map;
    const Ticks end{kPpq * 4 * 300};
    map.setTempo(Ticks{0}, 60.0, true);
    map.setTempo(end, 180.0, false);

    const double slope = (180.0 - 60.0) / static_cast<double>(end.value);
    const double analytic = (60.0 / (static_cast<double>(kPpq) * slope)) * std::log(180.0 / 60.0);

    const double measured = map.secondsAt(end);
    INFO("analytic " << analytic << " measured " << measured);
    CHECK(std::abs(measured - analytic) * kRate < 1.0);

    // And the two directions still agree at the far end.
    CHECK(map.toTicks(map.toSamples(end, kRate), kRate) == end);
}

TEST_CASE("a constant tempo matches the textbook formula", "[tempomap]") {
    TempoMap map;
    map.setTempo(Ticks{0}, 120.0, false);
    // One bar of 4/4 at 120 bpm is two seconds.
    CHECK_THAT(map.secondsAt(Ticks{kPpq * 4}), Catch::Matchers::WithinAbs(2.0, 1e-9));
    CHECK(map.toSamples(Ticks{kPpq * 4}, kRate).value == 96000);
}

TEST_CASE("tempomap_barbeat", "[tempomap]") {
    TempoMap map;
    map.setMeter(Ticks{0}, 4, 4);
    map.setMeter(map.fromBarBeat(BarBeatTick{.bar = 8, .beat = 0, .tick = 0}), 7, 8);
    map.setMeter(map.fromBarBeat(BarBeatTick{.bar = 16, .beat = 0, .tick = 0}), 5, 4);
    map.setMeter(map.fromBarBeat(BarBeatTick{.bar = 24, .beat = 0, .tick = 0}), 4, 4);

    std::mt19937 rng(99U);
    std::uniform_int_distribution<std::int64_t> ticks(0, kPpq * 4 * 200);
    for (int i = 0; i < 20000; ++i) {
        const Ticks at{ticks(rng)};
        const BarBeatTick position = map.toBarBeat(at);
        if (map.fromBarBeat(position) != at) {
            INFO("tick " << at.value << " -> " << position.bar << ':' << position.beat << ':'
                         << position.tick);
            FAIL();
        }
    }

    // 7/8 really is seven eighth notes, not seven quarters.
    const Ticks barNine = map.fromBarBeat(BarBeatTick{.bar = 9, .beat = 0, .tick = 0});
    const Ticks barEight = map.fromBarBeat(BarBeatTick{.bar = 8, .beat = 0, .tick = 0});
    CHECK((barNine - barEight).value == 7 * (kPpq / 2));
}

TEST_CASE("bar numbers are zero-based and negative positions count backwards", "[tempomap]") {
    const TempoMap map;
    CHECK(map.toBarBeat(Ticks{0}) == BarBeatTick{.bar = 0, .beat = 0, .tick = 0});
    CHECK(map.fromBarBeat(BarBeatTick{.bar = 8, .beat = 0, .tick = 0}).value == kPpq * 4 * 8);

    // A pickup: one tick before the downbeat is the last tick of bar -1, not of bar 0.
    const BarBeatTick before = map.toBarBeat(Ticks{-1});
    CHECK(before.bar == -1);
    CHECK(map.fromBarBeat(before).value == -1);
}

TEST_CASE("the map always has an event at tick zero", "[tempomap]") {
    TempoMap map;
    CHECK(map.tempoEvents().size() == 1);
    CHECK_FALSE(map.removeTempo(Ticks{0}));
    CHECK_FALSE(map.removeMeter(Ticks{0}));

    map.setTempo(Ticks{kPpq}, 140.0, false);
    CHECK(map.tempoEvents().size() == 2);
    CHECK(map.removeTempo(Ticks{kPpq}));
    CHECK(map.tempoEvents().size() == 1);
}

TEST_CASE("tempo is clamped rather than rejected", "[tempomap]") {
    TempoMap map;
    map.setTempo(Ticks{0}, 100000.0, false);
    CHECK(map.bpmAt(Ticks{0}) == adx::core::kMaxBpm);
    map.setTempo(Ticks{0}, -5.0, false);
    CHECK(map.bpmAt(Ticks{0}) == adx::core::kMinBpm);
}
