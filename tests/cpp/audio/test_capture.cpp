// The capture tap and the xrun count (phase_10.5.md §3 step 1, §5): what an agent hears a
// running engine with, and how it knows the stream kept up.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#include "engine/audio/CaptureTap.h"
#include "engine/audio/NullBackend.h"
#include "engine/format/audio/AudioFileLoader.h"
#include "engine/render/OfflineRender.h"
#include "engine/render/RenderEngine.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/Env.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

using namespace std::chrono_literals;

adx::project::Project song() {
    std::vector<adx::tests::NoteSpec> notes;
    for (int i = 0; i < 32; ++i) {
        notes.push_back({.start = i * (adx::core::kPpq / 2),
                         .length = adx::core::kPpq / 3,
                         .pitch = static_cast<std::uint8_t>(57 + ((i * 5) % 17)),
                         .velocity = 100});
    }
    return adx::tests::toneProject(notes, 120.0);
}

struct Running {
    std::unique_ptr<adx::render::RenderEngine> engine;
    adx::audio::NullBackend* null{nullptr};
};

Running start(const adx::project::Project& project) {
    Running r;
    auto backend = std::make_unique<adx::audio::NullBackend>();
    r.null = backend.get();
    r.engine = std::make_unique<adx::render::RenderEngine>(
        std::move(backend),
        adx::render::EngineOptions{.sampleRate = 48000, .blockFrames = 256, .outputChannels = 2});
    REQUIRE(r.engine->open().code == adx::audio::Error::Code::None);
    REQUIRE(r.engine->setProject(project, 0).rebuilt);
    r.engine->setLoop(adx::transport::LoopRegion{});
    r.engine->seek(adx::core::Ticks{0});
    r.engine->play();
    return r;
}

void runFor(Running& r, std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(5ms);
        r.engine->pump();
    }
}

} // namespace

TEST_CASE("capture_tap_matches_offline", "[audio][capture]") {
    // A capture of a null-audio engine is the offline render, sample for sample: the
    // tap sees exactly what the device was given, in order, with nothing lost.
    const adx::project::Project project = song();
    Running r = start(project);
    adx::audio::CaptureTap tap(r.engine->audio().scopeTap());
    tap.start(); // before the stream: the capture's first frame is the stream's first
    REQUIRE(r.engine->start().code == adx::audio::Error::Code::None);
    runFor(r, 2500ms);
    r.engine->stop();
    const adx::audio::Capture capture = tap.stop();
    CHECK(capture.lost == 0);
    CHECK(capture.startFrame == 0);
    REQUIRE(capture.frames > 48000 * 2);

    adx::render::RenderStats stats;
    const std::vector<float> offline =
        adx::tests::renderFrames(project, 256, capture.frames, stats);
    REQUIRE(offline.size() == capture.samples.size());
    CHECK(offline == capture.samples);

    // And it writes a WAV that decodes back to the same samples.
    const auto path = std::filesystem::temp_directory_path() / "adx_capture_test.wav";
    std::string error;
    REQUIRE(adx::audio::writeCaptureWav(capture, 48000, path, error));
    adx::format::SampleBuffer decoded;
    REQUIRE(adx::format::decodeFile(path, decoded, error));
    CHECK(decoded.frames == capture.frames);
    CHECK(decoded.leftAudio()[1000] == capture.samples[2000]);
    std::filesystem::remove(path);
}

TEST_CASE("capture_tap_no_alloc", "[audio][capture]") {
    // The capture adds nothing to the callback: under the allocator hook, with a
    // capture running, the audio thread allocates nothing. Five seconds here; the
    // [.slow] run below holds it for sixty.
    const auto seconds = adx::tests::environment("ADX_FULL_EVIDENCE") ? 60 : 5;
    Running r = start(song());
    adx::audio::CaptureTap tap(r.engine->audio().scopeTap());
    tap.start();
    const std::uint64_t before = adx::rt::ViolationLog::instance().count();
    REQUIRE(r.engine->start().code == adx::audio::Error::Code::None);
    runFor(r, std::chrono::seconds(seconds));
    r.engine->stop();
    const adx::audio::Capture capture = tap.stop();
    CHECK(adx::rt::ViolationLog::instance().count() == before);
    CHECK(capture.lost == 0);
    CHECK(capture.frames > static_cast<std::uint64_t>(48000 * (seconds - 1)));
}

TEST_CASE("xruns_exposed", "[audio][capture]") {
    // A clean run leaves the xrun count at zero; an induced overload - each callback
    // made to take twice its period - raises it.
    Running r = start(song());
    REQUIRE(r.engine->start().code == adx::audio::Error::Code::None);
    runFor(r, 1000ms);
    const std::uint64_t clean = r.engine->audio().info().xrunCount;
    if (!adx::tests::environment("CI")) {
        CHECK(clean == 0); // a shared runner deschedules threads; locally it is held
    }
    r.engine->audio().callbacks().setStallMicroseconds(12000); // a 5.3 ms period
    runFor(r, 1000ms);
    r.engine->audio().callbacks().setStallMicroseconds(0);
    const std::uint64_t overloaded = r.engine->audio().info().xrunCount;
    r.engine->stop();
    CHECK(overloaded > clean + 10);
}
