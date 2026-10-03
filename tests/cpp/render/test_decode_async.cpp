// phase_4.md §6, decode_async_does_not_block: a 200 MB file decodes without a callback
// exceeding its deadline. Decoding happens on the pool's workers; this proves nothing
// about it reaches the audio thread - not a lock it waits on, not a page it faults in.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>

#include "engine/audio/NullBackend.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/render/RenderEngine.h"
#include "engine/rt/AllocGuard.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/Env.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

/// A mono float WAV of `frames` frames of low-level noise, written in chunks so the
/// test itself does not hold 200 MB.
std::filesystem::path writeLargeWav(std::uint64_t frames) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "adx_decode_200mb.wav";
    std::ofstream out(path, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(frames * 4);
    out.write("RIFF", 4);
    u32(36 + bytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(3);
    u16(1);
    u32(48000);
    u32(48000 * 4);
    u16(4);
    u16(32);
    out.write("data", 4);
    u32(bytes);
    std::vector<float> chunk(1U << 16U);
    std::uint32_t state = 1;
    for (std::uint64_t written = 0; written < frames; written += chunk.size()) {
        for (float& s : chunk) {
            state = (state * 1664525U) + 1013904223U;
            s = (static_cast<float>(state >> 8U) / 16777216.0F - 0.5F) * 0.01F;
        }
        const std::uint64_t n = std::min<std::uint64_t>(chunk.size(), frames - written);
        out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(n * 4));
    }
    return path;
}

} // namespace

TEST_CASE("decode_async_does_not_block", "[render][decode][.slow]") {
    constexpr std::uint64_t kFrames = 52'500'000; // 210 MB of float
    const std::filesystem::path file = writeLargeWav(kFrames);

    // Something for the audio thread to do while the decode runs: the realtime gate's
    // own load, smaller.
    adx::tests::SyntheticSpec spec;
    spec.channels = 16;
    spec.notes = 4000;
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

    adx::format::SamplePool pool(2);
    const auto started = std::chrono::steady_clock::now();
    const auto entry = pool.request(file);
    while (entry->handle().state() == adx::format::SampleState::Loading) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        engine.pump();
        REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::minutes(5));
    }
    // A little more playback after the decode lands, for good measure.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        engine.pump();
    }
    engine.stop();

    REQUIRE(entry->handle().ready());
    CHECK(entry->handle().view().frames == kFrames);

    const auto& callbacks = engine.audio().callbacks();
    const double deadlineMs = 1000.0 * 256.0 / 48000.0;
    const double worstMs = static_cast<double>(callbacks.worstCallbackNs()) / 1e6;
    INFO("worst callback " << worstMs << " ms against " << deadlineMs << " ms; "
                           << callbacks.overDeadlineCount() << " over of "
                           << engine.audio().info().callbackCount);
    if (adx::tests::deadlineSlackAllowed()) {
        CHECK(callbacks.overDeadlineCount() * 100 <= engine.audio().info().callbackCount);
    } else {
        CHECK(worstMs < deadlineMs);
        CHECK(callbacks.overDeadlineCount() == 0);
    }
    if (adx::rt::allocGuardCompiledIn()) {
        CHECK(adx::rt::ViolationLog::instance().count() == before);
    }
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
}
