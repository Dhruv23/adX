// The gate.
//
// Everything else in this phase exists so that this test can be believed: a real
// stream, running for a real minute, through the real callback entry point, with the
// allocator hook armed. Zero violations, or the phase is not done.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "engine/audio/AudioThread.h"
#include "engine/audio/NullBackend.h"
#include "engine/audio/OfflineBackend.h"
#include "engine/audio/RtAudioBackend.h"
#include "engine/rt/AllocGuard.h"
#include "engine/rt/Denormal.h"
#include "engine/rt/LockGuardCheck.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/ThreadId.h"
#include "engine/rt/Violation.h"

using adx::audio::AudioThread;
using adx::audio::Error;
using adx::audio::NullBackend;
using adx::audio::OfflineBackend;
using adx::audio::RtAudioBackend;
using adx::audio::StreamConfig;
using adx::rt::ViolationKind;
using adx::rt::ViolationLog;

TEST_CASE("null_backend_runs_and_produces_silence", "[rt][audio]") {
    AudioThread audio{std::make_unique<NullBackend>()};
    ViolationLog::instance().reset();

    const StreamConfig config{.sampleRate = 48000, .blockFrames = 256};
    REQUIRE(audio.open(config).code == Error::Code::None);
    REQUIRE(audio.start().code == Error::Code::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    audio.stop();

    const auto info = audio.info();
    CHECK(info.callbackCount > 0);
    CHECK(info.backendName.view() == "Null");
    CHECK(info.outputLatencyMs > 0.0);
    CHECK(ViolationLog::instance().count() == 0);

    // The taps ran, over silence, which is what proves they are on the callback path
    // rather than waiting to be wired up in Phase 6.
    CHECK(audio.scopeTap().framesWritten() > 0);
    std::vector<adx::rt::StereoFrame> window(64);
    const std::size_t got = audio.scopeTap().readLatest(window.data(), window.size());
    REQUIRE(got > 0);
    for (std::size_t i = 0; i < got; ++i) {
        CHECK(window[i].left == 0.0F);
        CHECK(window[i].right == 0.0F);
    }
}

TEST_CASE("audio_thread_installs_guards", "[rt][audio]") {
    // Proves the guards are installed *by the callback path*, not by the test: a
    // process step that reports what the ambient realtime state looked like from
    // inside a real callback.
    struct Observed {
        std::atomic<bool> ran{false};
        std::atomic<bool> inRtSection{false};
        std::atomic<bool> denormalsFlushed{false};
        std::atomic<bool> isAudioThread{false};
        std::atomic<std::uint32_t> channels{0};
        std::atomic<std::uint32_t> frames{0};
    };
    Observed observed;

    AudioThread audio{std::make_unique<NullBackend>()};
    audio.setProcessStep(
        [](void* user, float* /*out*/, const float* /*in*/, std::uint32_t frames,
           std::uint32_t channels, const adx::audio::StreamTime& /*time*/) noexcept {
            auto* seen = static_cast<Observed*>(user);
            seen->inRtSection.store(adx::rt::inRtSection(), std::memory_order_relaxed);
            seen->denormalsFlushed.store(adx::rt::denormalsAreFlushed(), std::memory_order_relaxed);
            seen->isAudioThread.store(adx::rt::isAudioThread(), std::memory_order_relaxed);
            seen->channels.store(channels, std::memory_order_relaxed);
            seen->frames.store(frames, std::memory_order_relaxed);
            seen->ran.store(true, std::memory_order_release);
        },
        &observed);

    const StreamConfig config{.sampleRate = 48000, .blockFrames = 128};
    REQUIRE(audio.open(config).code == Error::Code::None);
    REQUIRE(audio.start().code == Error::Code::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    audio.stop();

    REQUIRE(observed.ran.load(std::memory_order_acquire));
    CHECK(observed.isAudioThread.load(std::memory_order_relaxed));

    // The RT section is compiled out in Release, where inRtSection() is a constexpr
    // false and ScopedRtSection has an empty body. Asserting it there would be
    // asserting that the guard we deliberately removed is still present.
    if (adx::rt::allocGuardCompiledIn()) {
        CHECK(observed.inRtSection.load(std::memory_order_relaxed));
    }
    CHECK(observed.frames.load(std::memory_order_relaxed) == 128);
    CHECK(observed.channels.load(std::memory_order_relaxed) == 2);
#if ADX_HAS_SSE_DENORMAL_CONTROL
    CHECK(observed.denormalsFlushed.load(std::memory_order_relaxed));
#endif

    // And every guard was left the way it was found - the section exited, the MXCSR
    // restored, the thread claim dropped.
    CHECK_FALSE(adx::rt::inRtSection());
    CHECK_FALSE(adx::rt::denormalsAreFlushed());
    CHECK_FALSE(adx::rt::isAudioThread());
}

TEST_CASE("process_step_receives_cleared_buffer", "[rt][audio]") {
    // render() clears before calling the step, so a graph that fills only some
    // channels leaves silence rather than whatever the device buffer last held.
    struct Seen {
        std::atomic<bool> allZero{false};
        std::atomic<bool> ran{false};
    };
    Seen seen;

    AudioThread audio{std::make_unique<OfflineBackend>()};
    audio.setProcessStep(
        [](void* user, float* out, const float* /*in*/, std::uint32_t frames,
           std::uint32_t channels, const adx::audio::StreamTime& /*time*/) noexcept {
            auto* observed = static_cast<Seen*>(user);
            const std::size_t count = static_cast<std::size_t>(frames) * channels;
            bool zero = true;
            for (std::size_t i = 0; i < count; ++i) {
                if (out[i] != 0.0F) {
                    zero = false;
                    break;
                }
            }
            observed->allZero.store(zero, std::memory_order_relaxed);
            // Write something, so the next callback would see it if clearing stopped.
            std::fill_n(out, count, 0.5F);
            observed->ran.store(true, std::memory_order_release);
        },
        &seen);

    auto& offline = static_cast<OfflineBackend&>(audio.backend());
    const StreamConfig config{.sampleRate = 48000, .blockFrames = 64};
    REQUIRE(audio.open(config).code == Error::Code::None);
    REQUIRE(audio.start().code == Error::Code::None);
    static_cast<void>(offline.renderFrames(std::uint64_t{64} * 8));

    CHECK(seen.ran.load(std::memory_order_acquire));
    CHECK(seen.allZero.load(std::memory_order_relaxed));
}

TEST_CASE("denormal_guard_sets_and_restores", "[rt]") {
    const bool before = adx::rt::denormalsAreFlushed();
    {
        const adx::rt::ScopedFlushDenormals guard;
        CHECK(adx::rt::denormalsAreFlushed());
    }
    CHECK(adx::rt::denormalsAreFlushed() == before);
}

TEST_CASE("lock_check_records_on_rt_thread", "[rt]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("RT guard not compiled in (Release)");
        return;
    }

    adx::rt::RtCheckedMutex mutex;
    ViolationLog& log = ViolationLog::instance();

    log.reset();
    {
        const adx::rt::ScopedRtSection section;
        mutex.lock();
        mutex.unlock();
    }
    CHECK(log.count(ViolationKind::Lock) == 1);

    log.reset();
    {
        const adx::rt::ScopedRtSection section;
        // Always legal, even on the audio thread: it cannot block, so it cannot cause
        // the inversion that makes lock() a violation.
        CHECK(mutex.tryLock());
        mutex.unlock();
    }
    CHECK(log.count(ViolationKind::Lock) == 0);

    log.reset();
    mutex.lock();
    mutex.unlock();
    CHECK(log.count(ViolationKind::Lock) == 0);
}

TEST_CASE("offline_backend_produces_identical_silence", "[rt][audio]") {
    // The two backends must agree sample for sample. They share AudioThread::render,
    // so this is really a test that nothing in the *driving* of the callback changes
    // the output - which is the property Phase 3's bit-identical offline-vs-realtime
    // guarantee is built on, asserted here while it is still trivially true and
    // therefore while a regression would be obvious.
    auto offlineBackend = std::make_unique<OfflineBackend>();
    OfflineBackend& offline = *offlineBackend;
    AudioThread offlineThread{std::move(offlineBackend)};

    const StreamConfig config{.sampleRate = 48000, .blockFrames = 256};
    REQUIRE(offlineThread.open(config).code == Error::Code::None);
    REQUIRE(offlineThread.start().code == Error::Code::None);

    const auto rendered = offline.renderFrames(48000);
    REQUIRE(rendered.size() == std::size_t{48000} * 2);
    CHECK(std::ranges::all_of(rendered, [](float sample) { return sample == 0.0F; }));

    AudioThread nullThread{std::make_unique<NullBackend>()};
    ViolationLog::instance().reset();
    REQUIRE(nullThread.open(config).code == Error::Code::None);
    REQUIRE(nullThread.start().code == Error::Code::None);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    nullThread.stop();

    std::vector<adx::rt::StereoFrame> window(256);
    const std::size_t got = nullThread.scopeTap().readLatest(window.data(), window.size());
    REQUIRE(got > 0);
    for (std::size_t i = 0; i < got; ++i) {
        CHECK(window[i].left == 0.0F);
        CHECK(window[i].right == 0.0F);
    }
    CHECK(ViolationLog::instance().count() == 0);
}

TEST_CASE("null_backend_60s_zero_violations", "[rt][audio][.slow]") {
    // Sixty seconds, at 48 kHz and 256 frames, under the allocator hook.
    //
    // The violation count is deterministic and is the real assertion. The callback
    // count is held to a tolerance instead: NullBackend derives every deadline from
    // the stream origin rather than from the previous wake-up, so an overshoot is
    // absorbed by the next iteration instead of accumulating - which is what lets the
    // count be asserted at all on an OS whose timer granularity is coarser than a
    // block.
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("RT guard not compiled in (Release); this gate is meaningless here");
        return;
    }

    AudioThread audio{std::make_unique<NullBackend>()};
    ViolationLog::instance().reset();

    const StreamConfig config{.sampleRate = 48000, .blockFrames = 256};
    REQUIRE(audio.open(config).code == Error::Code::None);
    REQUIRE(audio.start().code == Error::Code::None);

    const auto startedAt = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(60));
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
    audio.stop();

    const auto info = audio.info();
    const double expected = elapsed * 48000.0 / 256.0;
    const auto actual = static_cast<double>(info.callbackCount);
    const double errorFraction = std::abs(actual - expected) / expected;

    INFO("elapsed " << elapsed << " s, expected ~" << expected << " callbacks, got " << actual);
    CHECK(errorFraction < 0.01);

    // The gate.
    CHECK(ViolationLog::instance().count() == 0);
    CHECK(audio.arenaHighWaterMark() == 0); // nothing uses the arena until Phase 3
}

TEST_CASE("rtaudio_enumerate_does_not_crash", "[rt][audio][.device]") {
    RtAudioBackend backend;
    const auto devices = backend.enumerate();
    if (devices.empty()) {
        SUCCEED("no audio device on this machine (CI runners have none)");
        return;
    }
    for (const auto& device : devices) {
        CHECK_FALSE(device.name.empty());
        CHECK_FALSE(device.apiName.empty());
        CHECK((device.maxOutputChannels > 0 || device.maxInputChannels > 0));
    }
}
