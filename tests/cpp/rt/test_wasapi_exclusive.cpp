// WASAPI exclusive mode (STATE.md P9-9): what it refuses without touching a device,
// and, on a machine with one, that it renders at the device period and the onset
// probe the latency tool reads reports a play time.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "engine/audio/WasapiExclusiveBackend.h"

using adx::audio::Error;
using adx::audio::StreamConfig;
using adx::audio::StreamTime;
using adx::audio::WasapiExclusiveBackend;

namespace {

/// Silence until `loud` is set, then a constant level.
struct Signal {
    std::atomic<bool> loud{false};
};

void render(void* user, float* out, const float* /*in*/, std::uint32_t frames,
            const StreamTime& /*time*/) noexcept {
    const float level = static_cast<Signal*>(user)->loud.load() ? 0.25F : 0.0F;
    std::fill_n(out, static_cast<std::size_t>(frames) * 2U, level);
}

} // namespace

TEST_CASE("wasapi_exclusive_refuses_input_and_other_devices", "[rt][audio]") {
    Signal signal;
    {
        WasapiExclusiveBackend backend;
        StreamConfig config;
        config.inputChannels = 2;
        config.inputDeviceId = adx::audio::kDefaultDeviceId;
        const Error error = backend.open(config, &render, &signal);
        CHECK(error.code != Error::Code::None);
        CHECK_FALSE(backend.info().isOpen);
    }
    {
        WasapiExclusiveBackend backend;
        StreamConfig config;
        config.outputDeviceId = 3;
        CHECK(backend.open(config, &render, &signal).code != Error::Code::None);
    }
    {
        WasapiExclusiveBackend backend;
        CHECK(backend.open(StreamConfig{}, nullptr, nullptr).code == Error::Code::InvalidConfig);
        CHECK(backend.onsetPlayTime() == 0);
    }
}

TEST_CASE("wasapi_exclusive_renders_at_device_period", "[rt][audio][.device]") {
    Signal signal;
    WasapiExclusiveBackend backend;
    StreamConfig config;
    config.blockFrames = 64;
    const Error opened = backend.open(config, &render, &signal);
    if (opened.code != Error::Code::None) {
        // No device, one that refuses exclusive mode, or one another app holds.
        SUCCEED(std::string("exclusive mode unavailable: ") + std::string(opened.message.view()));
        return;
    }
    // The block is the device period: never below what was asked for in frames here,
    // since every driver's minimum period is longer than 64 frames at 48 kHz.
    CHECK(backend.info().blockFrames >= 64);
    CHECK(backend.info().outputLatencyMs > 0.0);
    CHECK_FALSE(backend.formatName().empty());
    REQUIRE(backend.start().code == Error::Code::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    backend.armOnset(0.1F);
    signal.loud.store(true);
    std::int64_t onset = 0;
    for (int i = 0; i < 100 && onset == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        onset = backend.onsetPlayTime();
    }
    backend.stop();
    CHECK(onset > 0);
    CHECK(backend.info().callbackCount > 10);
    CHECK(backend.info().xrunCount == 0);
    backend.close();
    CHECK_FALSE(backend.info().isOpen);
}
