// Recording (phase_8.md §3.1, §5), through a simulated loopback: the offline backend
// feeds the output back to the input a fixed number of frames later, which is what an
// interface with its outputs cabled to its inputs does - and the round trip a driver
// reports, or a calibration measures, is that number.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/audio/OfflineBackend.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/audio/AudioFileLoader.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/record/DiskWriter.h"
#include "engine/record/InputCapture.h"
#include "engine/record/RecordSession.h"
#include "engine/render/OfflineRender.h"
#include "engine/render/RenderEngine.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/clip/ClipFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace fs = std::filesystem;
using adx::tests::ClipDir;

namespace {

constexpr std::uint32_t kRate = 48000;

/// A render engine on an offline backend with two inputs wired back from the outputs.
struct LoopbackRig {
    adx::audio::OfflineBackend* backend{nullptr};
    std::unique_ptr<adx::render::RenderEngine> engine;

    LoopbackRig(std::uint32_t block, std::uint32_t delay) {
        auto owned = std::make_unique<adx::audio::OfflineBackend>();
        backend = owned.get();
        engine = std::make_unique<adx::render::RenderEngine>(
            std::move(owned), adx::render::EngineOptions{.sampleRate = kRate,
                                                         .blockFrames = block,
                                                         .outputChannels = 2,
                                                         .inputChannels = 2});
        backend->setLoopback(delay);
        REQUIRE(engine->open().code == adx::audio::Error::Code::None);
        REQUIRE(engine->start().code == adx::audio::Error::Code::None);
    }

    /// Hands `loaded` to the engine and waits until its audio clips are decoded. A clip
    /// plays a silent stand-in until then, which a device clock never notices but an
    /// optimised offline render of a whole take can outrun.
    void load(adx::tests::Loaded& loaded) const {
        REQUIRE(engine->setProject(loaded.project, loaded.stack.revision()).rebuilt);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (engine->audioPending()) {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            static_cast<void>(engine->commit(loaded.project, loaded.stack));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    /// Renders `frames`, a whole number of blocks.
    void run(std::uint64_t frames) const {
        static_cast<void>(backend->renderFrames(frames));
        engine->pump();
    }
};

/// A click at one second, on a playlist track of its own, and an empty track to record on.
std::unique_ptr<adx::tests::Loaded> clickProject(const ClipDir& dir) {
    std::vector<float> click(static_cast<std::size_t>(2 * kRate) * 2, 0.0F);
    for (std::size_t i = 0; i < 8; ++i) {
        click[(kRate + i) * 2] = click[((kRate + i) * 2) + 1] = 0.9F;
    }
    const fs::path file = dir.write("click.wav", click, kRate);
    return adx::tests::loadText(
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\n"
        "TRACK 1 name=\"Click\"\n  AUDIO \"" +
        file.generic_string() +
        "\" 0:0:0\nTRACK 2 name=\"Rec\"\n\n[MIXER]\nINSERT 1 name=\"Master\"\n");
}

/// A take on `track`, written under `dir`, moved back by `roundTrip`.
adx::record::RecordSettings settingsFor(adx::core::PlaylistTrackId track, const fs::path& dir,
                                        std::optional<std::int64_t> roundTrip) {
    adx::record::RecordSettings settings;
    settings.track = track;
    settings.projectDirectory = dir;
    settings.measuredRoundTrip = roundTrip;
    return settings;
}

std::size_t firstAbove(const std::vector<float>& stereo, float level, std::size_t from = 0) {
    for (std::size_t f = from; (f * 2) < stereo.size(); ++f) {
        if (std::abs(stereo[f * 2]) > level) {
            return f;
        }
    }
    return stereo.size() / 2;
}

} // namespace

TEST_CASE("capture_no_alloc", "[record][rt]") {
    // Sixty seconds of capture on the audio thread, under the allocator hook where the
    // build has one, with a real writer draining behind it.
    ClipDir const dir;
    LoopbackRig const rig(256, 512);
    const auto loaded = clickProject(dir);
    rig.load(*loaded);
    // On the heap: its ring is 4 MB.
    auto owned = std::make_unique<adx::record::InputCapture>();
    adx::record::InputCapture& capture = *owned;
    adx::record::DiskWriter writer(capture, dir.path() / "long.wav", kRate,
                                   adx::record::WriterWindow{});
    std::string error;
    REQUIRE(writer.start(error));
    rig.engine->core().setCapture(&capture);
    capture.arm(true);
    rig.engine->play();
    const std::uint64_t before = adx::rt::ViolationLog::instance().count();
    for (int second = 0; second < 60; ++second) {
        rig.run(kRate);
    }
    CHECK(adx::rt::ViolationLog::instance().count() == before);
    rig.engine->core().setCapture(nullptr);
    REQUIRE(writer.stop(error));
    CHECK(capture.overrunCount() == 0);
    CHECK(writer.framesWritten() == 60ULL * kRate);
}

TEST_CASE("capture_overrun_counted", "[record]") {
    ClipDir const dir;
    LoopbackRig const rig(256, 512);
    const auto loaded = clickProject(dir);
    rig.load(*loaded);
    // On the heap: its ring is 4 MB.
    auto owned = std::make_unique<adx::record::InputCapture>();
    adx::record::InputCapture& capture = *owned;
    adx::record::DiskWriter writer(capture, dir.path() / "stalled.wav", kRate,
                                   adx::record::WriterWindow{});
    std::string error;
    REQUIRE(writer.start(error));
    writer.setStalled(true); // a disk that cannot keep up
    rig.engine->core().setCapture(&capture);
    capture.arm(true);
    rig.engine->play();
    for (int second = 0; second < 15; ++second) {
        rig.run(kRate);
    }
    rig.engine->core().setCapture(nullptr);
    REQUIRE(writer.stop(error));
    // Counted, and the take stopped where the ring filled: everything captured was
    // written, nothing after the gap was, and the file is shorter than the run.
    CHECK(capture.overran());
    CHECK(writer.framesWritten() == capture.framesCaptured());
    CHECK(writer.framesWritten() <= adx::record::kCaptureRingSamples / 2);
    adx::format::SampleBuffer back;
    REQUIRE(adx::format::decodeFile(dir.path() / "stalled.wav", back, error));
    CHECK(back.frames == writer.framesWritten());
}

TEST_CASE("record_latency_alignment", "[record]") {
    // At three buffer sizes, a calibration measures the loopback's round trip, and a take
    // recorded with it has the click exactly where the click track has it.
    for (const std::uint32_t block : {64U, 256U, 1024U}) {
        INFO("block " << block);
        const std::uint32_t delay = (block * 2) + 37;
        ClipDir const dir;
        auto loaded = clickProject(dir);
        LoopbackRig const rig(block, delay);
        rig.load(*loaded);

        adx::record::Calibration calibration(*rig.engine);
        calibration.begin(loaded->project);
        rig.run(static_cast<std::uint64_t>(block) *
                (((kRate / 4) / block) + 1)); // past the ping at 0.1 s and its echo
        const std::optional<std::int64_t> measured = calibration.finish();
        REQUIRE(measured.has_value());
        CHECK(std::cmp_equal(*measured, delay));

        const adx::core::PlaylistTrackId rec = loaded->project.playlist.tracks[1].id;
        adx::record::RecordSession session(*rig.engine, loaded->project, loaded->stack,
                                           settingsFor(rec, dir.path(), measured));
        std::string error;
        REQUIRE(session.start(error));
        rig.run(block * ((2 * static_cast<std::size_t>(kRate)) / block + 4));
        const adx::record::TakeResult take = session.stop();
        INFO(take.error);
        REQUIRE(take.ok);

        // The click alone, then the take alone: the same sample.
        loaded->project.resources.baseDirectory = dir.path().string();
        adx::project::Project clickOnly = loaded->project;
        clickOnly.playlist.tracks[1].muted = true;
        adx::project::Project takeOnly = loaded->project;
        takeOnly.playlist.tracks[0].muted = true;
        adx::render::OfflineRenderOptions options;
        options.frames = 2 * static_cast<std::size_t>(kRate);
        options.blockFrames = block;
        adx::render::RenderStats stats;
        const std::size_t original =
            firstAbove(adx::render::renderOffline(clickOnly, options, stats), 0.4F);
        const std::size_t recorded =
            firstAbove(adx::render::renderOffline(takeOnly, options, stats), 0.4F);
        CHECK(original == kRate);
        CHECK(static_cast<std::int64_t>(recorded) - static_cast<std::int64_t>(original) >= -1);
        CHECK(static_cast<std::int64_t>(recorded) - static_cast<std::int64_t>(original) <= 1);
    }
}

TEST_CASE("record_punch_region", "[record]") {
    ClipDir const dir;
    // A steady tone to record, so every frame of the take has something in it.
    const fs::path tone =
        dir.write("tone.wav",
                  adx::tests::stereoSine(8 * static_cast<std::size_t>(kRate), kRate, 220.0), kRate);
    auto loaded = adx::tests::loadText(
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\n"
        "TRACK 1\n  AUDIO \"" +
        tone.generic_string() +
        "\" 0:0:0\nTRACK 2 name=\"Rec\"\n\n[MIXER]\nINSERT 1 name=\"Master\"\n");
    LoopbackRig const rig(256, 600);
    rig.load(*loaded);
    const adx::core::Ticks bar{adx::core::kPpq * 4};
    adx::record::RecordSettings punch =
        settingsFor(loaded->project.playlist.tracks[1].id, dir.path(), 600);
    punch.punchIn = bar;
    punch.punchOut = bar * 2;
    punch.countIn = {.bars = 1, .click = false};
    adx::record::RecordSession session(*rig.engine, loaded->project, loaded->stack, punch);
    std::string error;
    REQUIRE(session.start(error));
    // Pre-roll from bar 0, the punch over bar 1, a bar of post-roll after it.
    rig.run(256 * ((6 * static_cast<std::size_t>(kRate)) / 256));
    const adx::record::TakeResult take = session.stop();
    REQUIRE(take.ok);
    // Two seconds a bar at 120: exactly the punch, nothing of the pre- or post-roll.
    CHECK(take.frames == 2 * static_cast<std::size_t>(kRate));
    const adx::project::PlaylistItem& item = loaded->project.playlist.tracks[1].items.front();
    CHECK(item.start == bar);
    CHECK(item.length == bar);
    adx::format::SampleBuffer back;
    REQUIRE(adx::format::decodeFile(take.file, back, error));
    REQUIRE(back.frames == 2 * static_cast<std::size_t>(kRate));
    float quietest = 1.0F;
    for (std::size_t f = 100; f + 100 < back.frames; f += 997) {
        quietest =
            std::min(quietest, std::abs(back.leftAudio()[f]) + std::abs(back.leftAudio()[f + 50]));
    }
    CHECK(quietest > 0.0F);
}

TEST_CASE("record_takes_stack_on_lanes", "[record]") {
    ClipDir const dir;
    auto loaded = clickProject(dir);
    LoopbackRig const rig(256, 512);
    rig.load(*loaded);
    const adx::core::PlaylistTrackId rec = loaded->project.playlist.tracks[1].id;
    std::vector<adx::record::TakeResult> takes;
    for (int t = 0; t < 2; ++t) {
        adx::record::RecordSession session(*rig.engine, loaded->project, loaded->stack,
                                           settingsFor(rec, dir.path(), 512));
        std::string error;
        REQUIRE(session.start(error));
        rig.run(std::uint64_t{256} * 200);
        takes.push_back(session.stop());
        REQUIRE(takes.back().ok);
    }
    CHECK(takes[0].lane == 1);
    CHECK(takes[1].lane == 2);
    CHECK(takes[0].file != takes[1].file);
    const auto& items = loaded->project.playlist.tracks[1].items;
    REQUIRE(items.size() == 2);
    CHECK(items[0].muted);
    CHECK_FALSE(items[1].muted);
}

TEST_CASE("record_take_comping", "[record]") {
    ClipDir const dir;
    const std::array<std::vector<float>, 3> sources{
        adx::tests::stereoNoise(3 * static_cast<std::size_t>(kRate), 11),
        adx::tests::stereoNoise(3 * static_cast<std::size_t>(kRate), 22),
        adx::tests::stereoNoise(3 * static_cast<std::size_t>(kRate), 33)};
    std::string text = "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\nTRACK 1\n";
    for (std::size_t t = 0; t < sources.size(); ++t) {
        const fs::path take = dir.write("take" + std::to_string(t) + ".wav", sources[t], kRate);
        text += "  AUDIO \"" + take.generic_string() + "\" 0:0:0 lane=" + std::to_string(t + 1) +
                (t < 2 ? " mute=yes" : "") + "\n";
    }
    text += "\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    auto loaded = adx::tests::loadText(text);
    const auto& track = loaded->project.playlist.tracks.front();
    const adx::core::Ticks beat{adx::core::kPpq};
    // Beat 1 from take 1, beat 2 from take 2, beats 3-4 from take 3.
    const std::vector<adx::record::CompRange> ranges{
        {.from = beat * 0, .to = beat, .take = track.items[0].id},
        {.from = beat, .to = beat * 2, .take = track.items[1].id},
        {.from = beat * 2, .to = beat * 4, .take = track.items[2].id}};
    adx::record::comp(loaded->project, loaded->stack, track.id, ranges);

    adx::render::OfflineRenderOptions options;
    options.frames = 2 * static_cast<std::size_t>(kRate);
    adx::render::RenderStats stats;
    const std::vector<float> out = adx::render::renderOffline(loaded->project, options, stats);
    const std::size_t half = kRate / 2; // a beat
    // Every sample of every range is its take's sample at that time - right up to and
    // from each boundary.
    for (const std::size_t frame :
         {std::size_t{0}, half - 1, half, (2 * half) - 1, 2 * half, (4 * half) - 1}) {
        const std::size_t take = std::min<std::size_t>(frame / half, 2);
        INFO("frame " << frame);
        CHECK(out[frame * 2] == sources[take][frame * 2]);
        CHECK(out[(frame * 2) + 1] == sources[take][(frame * 2) + 1]);
    }
    // The takes are muted under the comp, and the whole comp is one undo step.
    const auto& after = loaded->project.playlist.tracks.front().items;
    CHECK(std::ranges::all_of(after, [](const auto& i) { return i.lane == 0 || i.muted; }));
    REQUIRE(loaded->stack.undo(loaded->project));
    CHECK(loaded->project.playlist.tracks.front().items.size() == 3);
}
