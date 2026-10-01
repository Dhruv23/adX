// One time source: advance, boundaries, loop wrap, seek, tempo rebind, readout.

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "engine/core/TempoMath.h"
#include "engine/transport/Seek.h"
#include "engine/transport/TimeSource.h"
#include "engine/transport/TransportSet.h"

using adx::core::kPpq;
using adx::core::Ticks;
using adx::transport::LoopRegion;
using adx::transport::PlayState;
using adx::transport::TimeSource;

namespace {

struct Tempo {
    std::vector<adx::core::TempoEvent> events;
    std::vector<double> cumSeconds;

    explicit Tempo(std::vector<adx::core::TempoEvent> list)
        : events(std::move(list)), cumSeconds(events.size()) {
        adx::core::computeCumulativeSeconds(events, cumSeconds);
    }
    [[nodiscard]] adx::core::TempoView view() const {
        return adx::core::TempoView{.events = events, .cumSeconds = cumSeconds};
    }
};

} // namespace

TEST_CASE("timesource_advances_only_while_rolling", "[transport]") {
    TimeSource time;
    static_cast<void>(time.beginBlock());
    CHECK_FALSE(time.advance(256));
    CHECK(time.positionSamples() == 0);

    time.requestState(PlayState::Playing);
    const auto started = time.beginBlock();
    CHECK(started.started);
    CHECK_FALSE(time.advance(256));
    CHECK(time.positionSamples() == 256);

    time.requestState(PlayState::Stopped);
    const auto stopped = time.beginBlock();
    CHECK(stopped.stopped);
    CHECK(adx::transport::effectsOf(stopped).releaseVoices);
    CHECK_FALSE(time.advance(256));
    CHECK(time.positionSamples() == 256);
}

TEST_CASE("timesource_wraps_on_the_exact_sample", "[transport][loop]") {
    TimeSource time; // 120 bpm, 48 kHz: one beat is 24000 samples
    time.requestLoop(LoopRegion{.start = Ticks{kPpq}, .end = Ticks{kPpq * 2}, .enabled = true});
    time.requestSeek(Ticks{kPpq + (kPpq / 2)});
    time.requestState(PlayState::Playing);
    static_cast<void>(time.beginBlock());
    REQUIRE(time.positionSamples() == 36000);

    // 12000 frames to the loop end: a 20000-frame block must be cut there.
    CHECK(time.framesToNextBoundary(20000) == 12000);
    CHECK(time.advance(12000));
    CHECK(time.positionSamples() == 24000);
    const std::uint64_t generation = time.seekGeneration();
    // A wrap is not a seek.
    CHECK(time.seekGeneration() == generation);
    CHECK(time.framesToNextBoundary(30000) == 24000);
}

TEST_CASE("timesource_seek_bumps_generation", "[transport][seek]") {
    TimeSource time;
    const std::uint64_t before = time.seekGeneration();
    time.requestSeek(Ticks{kPpq * 3});
    const auto transition = time.beginBlock();
    CHECK(transition.seeked);
    CHECK_FALSE(transition.wasRollingBeforeSeek);
    CHECK(time.seekGeneration() == before + 1);
    CHECK(time.positionTicks() == Ticks{kPpq * 3});
    // Seek while stopped: nothing is sounding, so nothing is released.
    CHECK_FALSE(adx::transport::effectsOf(transition).releaseVoices);

    time.requestState(PlayState::Playing);
    static_cast<void>(time.beginBlock());
    time.requestSeek(Ticks{0});
    const auto whilePlaying = time.beginBlock();
    CHECK(whilePlaying.wasRollingBeforeSeek);
    CHECK(adx::transport::effectsOf(whilePlaying).releaseVoices);
}

TEST_CASE("timesource_tempo_rebind_keeps_the_bar", "[transport]") {
    TimeSource time;
    time.requestSeek(Ticks{kPpq * 16});
    static_cast<void>(time.beginBlock());
    REQUIRE(time.positionSamples() == std::int64_t{16} * 24000);

    // Same content in different storage: nothing moves, nothing is invalidated.
    const Tempo same{{adx::core::TempoEvent{.at = Ticks{0}, .bpm = 120.0, .ramp = false}}};
    const std::uint64_t generation = time.seekGeneration();
    time.bindTempo(same.view(), 48000);
    CHECK(time.positionSamples() == std::int64_t{16} * 24000);
    CHECK(time.seekGeneration() == generation);

    // A real tempo change: the sample position moves so that the musical one does not.
    const Tempo faster{{adx::core::TempoEvent{.at = Ticks{0}, .bpm = 240.0, .ramp = false}}};
    time.bindTempo(faster.view(), 48000);
    CHECK(time.positionTicks() == Ticks{kPpq * 16});
    CHECK(time.positionSamples() == std::int64_t{16} * 12000);
    CHECK(time.seekGeneration() == generation + 1);
}

TEST_CASE("timesource_splits_at_tempo_change", "[transport]") {
    const Tempo tempo{{adx::core::TempoEvent{.at = Ticks{0}, .bpm = 120.0, .ramp = false},
                       adx::core::TempoEvent{.at = Ticks{kPpq}, .bpm = 60.0, .ramp = false}}};
    TimeSource time;
    time.bindTempo(tempo.view(), 48000);
    time.requestState(PlayState::Playing);
    static_cast<void>(time.beginBlock());
    CHECK(time.framesToNextBoundary(30000) == 24000);
    static_cast<void>(time.advance(24000));
    CHECK(time.framesToNextBoundary(30000) == 30000);
}

TEST_CASE("timesource_publishes_for_other_threads", "[transport]") {
    TimeSource time;
    time.requestSeek(Ticks{kPpq * 2});
    time.requestState(PlayState::Playing);
    static_cast<void>(time.beginBlock());
    time.publish();
    CHECK(time.publishedTicks() == kPpq * 2);
    CHECK(time.publishedSamples() == 48000);
    CHECK(time.publishedState() == PlayState::Playing);
}

TEST_CASE("transport_set_acquires_without_allocating", "[transport]") {
    adx::transport::TransportSet set;
    CHECK(set.inUseCount() == 1); // the arrangement
    std::vector<adx::transport::TimeSourceId> taken;
    for (std::uint32_t i = 1; i < adx::transport::kMaxTimeSources; ++i) {
        const auto id = set.acquire();
        REQUIRE(id != adx::transport::kInvalidTimeSource);
        taken.push_back(id);
    }
    CHECK(set.acquire() == adx::transport::kInvalidTimeSource);
    set.release(taken.front());
    CHECK(set.acquire() == taken.front());
    set.release(adx::transport::TimeSourceId{0});
    CHECK(set.inUse(adx::transport::TimeSourceId{0}));
}
