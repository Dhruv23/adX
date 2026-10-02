// Zero allocations, and arena headroom, under the phase gate's load.
//
// FINAL_PLAN §3.3.1 - the audio thread allocated 86 times a second - is closed only
// if the full 200-channel render, realtime and offline, records nothing under the
// allocator hook, and the per-callback arena that replaced those allocations is sized
// with room to spare rather than merely not overflowing today.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

#include "engine/audio/NullBackend.h"
#include "engine/rt/AllocGuard.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/Env.h"
#include "tests/cpp/render/RenderEvidence.h"
#include "tests/cpp/render/RenderFixtures.h"

TEST_CASE("render_no_alloc_under_load", "[render][gate][.slow]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("RT guard not compiled in (Release); zero here would prove nothing");
        return;
    }
    const adx::tests::RenderEvidence& evidence = adx::tests::loadEvidence();
    INFO(evidence.description);
    for (const adx::tests::RenderRun& run : evidence.runs) {
        INFO("block " << run.blockFrames);
        CHECK(run.offlineViolations == 0);
        CHECK(run.realtimeViolations == 0);
    }
}

TEST_CASE("arena_high_water_under_load", "[render][gate][.slow]") {
    const adx::tests::RenderEvidence& evidence = adx::tests::loadEvidence();
    INFO(evidence.description);
    for (const adx::tests::RenderRun& run : evidence.runs) {
        INFO("block " << run.blockFrames << ": high water " << run.arenaHighWater << " of "
                      << run.arenaCapacity);
        REQUIRE(run.arenaCapacity > 0);
        CHECK(run.arenaHighWater > 0); // the arena is genuinely in use now
        CHECK(static_cast<double>(run.arenaHighWater) <
              0.8 * static_cast<double>(run.arenaCapacity));
    }
}

TEST_CASE("realtime_gate_60s_under_load", "[render][gate][.slow]") {
    // Phase 1's sixty-second gate, given something to do (P1-3). Thirty-two channels
    // of test tones looping for a minute on a real audio thread, under the allocator
    // hook, with every callback timed: the assertion is on the *slowest* callback,
    // because a stream that makes its deadline on average while missing it regularly
    // is exactly what a dropout is.
    adx::tests::SyntheticSpec spec;
    spec.channels = 32;
    spec.notes = 16'000;
    const adx::project::Project project = adx::tests::syntheticProject(spec);

    auto backend = std::make_unique<adx::audio::NullBackend>();
    adx::render::RenderEngine engine{std::move(backend), adx::render::EngineOptions{}};
    REQUIRE(engine.open().code == adx::audio::Error::Code::None);
    REQUIRE(engine.setProject(project, 1).rebuilt);
    engine.setLoop(adx::transport::LoopRegion{
        .start = adx::core::Ticks{0}, .end = project.contentLength(), .enabled = true});
    engine.play();

    const std::uint64_t before = adx::rt::ViolationLog::instance().count();
    engine.audio().callbacks().resetTiming();
    REQUIRE(engine.start().code == adx::audio::Error::Code::None);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        engine.pump();
    }
    engine.stop();

    const auto& callbacks = engine.audio().callbacks();
    const double deadlineMs = 1000.0 * 256.0 / 48000.0;
    const double worstMs = static_cast<double>(callbacks.worstCallbackNs()) / 1e6;
    INFO("worst callback " << worstMs << " ms against a " << deadlineMs << " ms deadline; "
                           << callbacks.overDeadlineCount() << " over; "
                           << engine.audio().info().callbackCount << " callbacks");
    CHECK(engine.audio().info().callbackCount > 10'000);
    if (adx::tests::deadlineSlackAllowed()) {
        CHECK(callbacks.overDeadlineCount() * 100 <= engine.audio().info().callbackCount);
    } else {
        CHECK(worstMs < deadlineMs);
        CHECK(callbacks.overDeadlineCount() == 0);
    }
    if (adx::rt::allocGuardCompiledIn()) {
        CHECK(adx::rt::ViolationLog::instance().count() == before);
    }
}
