// The commit protocol: incremental rebuilds, swaps between blocks, reclamation, knob
// turns without rebuilds, and a queue that says so when it is full.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <thread>

#include "engine/audio/NullBackend.h"
#include "engine/graph/EngineCore.h"
#include "engine/project/SnapshotBuilder.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/rt/Reaper.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::project::SnapshotBuilder;

namespace {

bool sameTracks(const adx::project::Snapshot& a, const adx::project::Snapshot& b) {
    if (a.eventTracks.size() != b.eventTracks.size()) {
        return false;
    }
    for (std::size_t t = 0; t < a.eventTracks.size(); ++t) {
        const auto& lhs = a.eventTracks[t].events;
        const auto& rhs = b.eventTracks[t].events;
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t e = 0; e < lhs.size(); ++e) {
            if (lhs[e].tick != rhs[e].tick || lhs[e].endTick != rhs[e].endTick ||
                lhs[e].noteId != rhs[e].noteId || lhs[e].instance != rhs[e].instance ||
                lhs[e].kind != rhs[e].kind || lhs[e].pitch != rhs[e].pitch ||
                lhs[e].velocity != rhs[e].velocity) {
                return false;
            }
        }
    }
    return true;
}

float peakBetween(const std::vector<float>& out, std::size_t from, std::size_t to) {
    float peak = 0.0F;
    for (std::size_t frame = from; frame < to; ++frame) {
        peak = std::max(peak, std::abs(out[frame * 2]));
    }
    return peak;
}

} // namespace

TEST_CASE("snapshot_incremental_budget", "[render][snapshot]") {
    // A single-note edit in a 100k-note project. It must not re-flatten the mixer, must
    // re-flatten exactly one channel's track, and must produce exactly what a build
    // from scratch would (phase_3.md §10: the debug mode that asserts it).
    adx::project::Project project = adx::tests::syntheticProject(adx::tests::SyntheticSpec{});
    adx::project::CommandStack stack;
    SnapshotBuilder builder{48000, 2048};

    auto first = builder.build(project, stack.revision(), adx::project::dirty::kAll);
    REQUIRE(first.snapshot != nullptr);
    adx::project::destroySnapshot(first.snapshot);

    adx::project::Pattern& pattern = project.patterns[17];
    const adx::project::NoteClip& clip = pattern.noteClips.front();
    stack.execute(std::make_unique<adx::project::SetNoteValue>(
                      pattern.id, clip.channel, std::vector<adx::core::NoteId>{clip.notes[5].id},
                      adx::project::NoteField::Pitch, 61.0),
                  project);
    const adx::project::DirtyMask dirty = stack.dirtySince(0);
    REQUIRE(dirty == adx::project::dirty::kPatterns);

    // Best of a few: the budget is about the algorithm, not about one unlucky
    // scheduling hiccup on a shared CI runner.
    double fastest = 1e9;
    adx::project::Snapshot* incremental = nullptr;
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto result = builder.build(project, stack.revision(), dirty);
        REQUIRE(result.snapshot != nullptr);
        const auto& stats = builder.lastStats();
        CHECK(stats.graphReused);
        CHECK(stats.tempoReused);
        CHECK(stats.tracksRebuilt == (attempt == 0 ? 1U : 0U));
        CHECK(stats.nodesCreated == 0);
        fastest = std::min(fastest, stats.milliseconds);
        if (incremental != nullptr) {
            adx::project::destroySnapshot(incremental);
        }
        incremental = result.snapshot;
    }

    SnapshotBuilder scratch{48000, 2048};
    auto full = scratch.build(project, stack.revision(), adx::project::dirty::kAll);
    REQUIRE(full.snapshot != nullptr);
    CHECK(sameTracks(*incremental, *full.snapshot));
    CHECK(std::ranges::equal(incremental->params, full.snapshot->params));
    adx::project::destroySnapshot(incremental);
    adx::project::destroySnapshot(full.snapshot);

    INFO("single-note rebuild: " << fastest << " ms");
    if (adx::tests::optimisedBuild()) {
        CHECK(fastest < 2.0);
    }
}

TEST_CASE("snapshot_swap_between_blocks", "[render][snapshot]") {
    // Ten thousand snapshots swapped in under a sustained low note, on a real audio
    // thread. A torn read - a half-installed snapshot, a cursor that replays a note-on,
    // a voice reset - shows up as a discontinuity in a signal that should be a clean
    // sine. The allocator hook watches the whole time.
    const adx::project::Project project = adx::tests::toneProject(
        {adx::tests::NoteSpec{.start = 0, .length = kPpq * 400, .pitch = 36}});

    auto backend = std::make_unique<adx::audio::NullBackend>();
    adx::audio::NullBackend& null = *backend;
    adx::render::RenderEngine engine{std::move(backend),
                                     adx::render::EngineOptions{.blockFrames = 64}};
    REQUIRE(engine.open().code == adx::audio::Error::Code::None);
    std::vector<float> capture(static_cast<std::size_t>(48000) * 60 * 2);
    null.setCapture(capture);
    REQUIRE(engine.setProject(project, 1).rebuilt);
    engine.play();

    const std::uint64_t violations = adx::rt::ViolationLog::instance().count();
    REQUIRE(engine.start().code == adx::audio::Error::Code::None);
    std::uint64_t revision = 2;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(50);
    while (engine.core().swapCount() < 10'000 && std::chrono::steady_clock::now() < deadline) {
        REQUIRE(engine.setProject(project, revision++).rebuilt);
        engine.pump();
        if (engine.backlog() > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }
    engine.stop();
    engine.pump();

    CHECK(engine.core().swapCount() >= 10'000);
    CHECK(adx::rt::ViolationLog::instance().count() == violations);

    const std::size_t frames = std::min<std::size_t>(null.capturedFrames(), capture.size() / 2);
    REQUIRE(frames > 48000);
    float largest = 0.0F;
    for (std::size_t frame = 1000; frame < frames; ++frame) {
        largest = std::max(largest, std::abs(capture[frame * 2] - capture[(frame - 1) * 2]));
    }
    INFO("largest step " << largest << " over " << frames << " frames");
    CHECK(largest < 0.01F);
    CHECK(peakBetween(capture, frames - 4800, frames) > 0.05F); // still sounding
}

TEST_CASE("reaper_reclaims_snapshots", "[render][snapshot]") {
    const std::int64_t before = adx::project::liveSnapshotCount();
    {
        const adx::project::Project project = adx::tests::toneProject({adx::tests::NoteSpec{}});
        const adx::tests::OfflineRig rig{64};
        rig.engine->play();
        for (std::uint64_t revision = 1; revision <= 10'000; ++revision) {
            REQUIRE(rig.engine->setProject(project, revision).rebuilt);
            static_cast<void>(rig.render(64));
        }
        rig.engine->pump();
        // One snapshot rendering, nothing waiting, nothing unreclaimed.
        CHECK(adx::project::liveSnapshotCount() - before == 1);
        CHECK(rig.engine->core().renderedRevision() == 10'000);
    }
    CHECK(adx::project::liveSnapshotCount() == before);
}

TEST_CASE("param_change_without_rebuild", "[render][snapshot]") {
    // A knob turned a thousand times is a thousand values on the queue and zero
    // snapshot rebuilds - and the audio hears the last one.
    auto loaded = adx::tests::loadText(R"([PROJECT]
ADX_VERSION=2

[CHANNEL Tone]
OUTPUT=insert.1

[PATTERN P]
LENGTH=64:0:0
NOTES Tone
  A2 0:0:0 64:0:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
)");
    const adx::tests::OfflineRig rig{256};
    REQUIRE(rig.engine->setProject(loaded->project, loaded->stack.revision()).rebuilt);
    rig.engine->play();
    const std::vector<float> before = rig.render(48000);
    const float loud = peakBetween(before, 24000, 48000);
    REQUIRE(loud > 0.05F);

    const adx::project::ParamRef volume{.owner = loaded->project.channels.front().id.value,
                                        .index = 0,
                                        .kind = adx::project::ParamKind::ChannelVolume};
    for (int turn = 0; turn < 1000; ++turn) {
        const float value = 1.0F - (0.75F * static_cast<float>(turn + 1) / 1000.0F);
        REQUIRE(rig.engine->setParam(loaded->project, loaded->stack, volume, value));
        static_cast<void>(rig.render(64));
        CHECK_FALSE(rig.engine->commit(loaded->project, loaded->stack).rebuilt);
    }
    CHECK(rig.engine->snapshotsBuilt() == 1);
    CHECK(loaded->project.channels.front().volume == 0.25F);

    const std::vector<float> after = rig.render(48000);
    const float quiet = peakBetween(after, 24000, 48000);
    INFO("peak before " << loud << ", after " << quiet);
    CHECK(std::abs(quiet - (loud * 0.25F)) < loud * 0.01F);

    // A structural edit after the knob turns still rebuilds, and keeps the value.
    loaded->stack.execute(
        std::make_unique<adx::project::SetNoteValue>(
            loaded->project.patterns.front().id, loaded->project.channels.front().id,
            std::vector<adx::core::NoteId>{
                loaded->project.patterns.front().noteClips.front().notes.front().id},
            adx::project::NoteField::Velocity, 90.0),
        loaded->project);
    CHECK(rig.engine->commit(loaded->project, loaded->stack).rebuilt);
}

TEST_CASE("engine_queue_refusal_is_recorded", "[render][snapshot]") {
    // Phase 1's P1-4: a full ring used to fail silently to the caller that mattered.
    // The engine's queue records the refusal as an Unbounded violation, and the render
    // engine keeps the message and sends it once there is room.
    adx::transport::TransportSet transport;
    adx::rt::Reaper reaper;
    adx::graph::EngineCore core{transport, reaper};
    adx::rt::ViolationLog::instance().reset();

    adx::graph::EngineMessage knob;
    knob.kind = adx::graph::MessageKind::Param;
    for (std::size_t i = 0; i < adx::graph::EngineCore::kQueueCapacity; ++i) {
        REQUIRE(core.post(knob));
    }
    CHECK(adx::rt::ViolationLog::instance().count(adx::rt::ViolationKind::Unbounded) == 0);
    CHECK_FALSE(core.post(knob));
    CHECK(adx::rt::ViolationLog::instance().count(adx::rt::ViolationKind::Unbounded) == 1);
    adx::rt::ViolationLog::instance().reset();

    // And through the engine: nothing is lost, it waits.
    const adx::tests::OfflineRig rig{64};
    const adx::project::Project project = adx::tests::toneProject({adx::tests::NoteSpec{}});
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    for (std::size_t i = 0; i < adx::graph::EngineCore::kQueueCapacity + 10; ++i) {
        rig.engine->seek(adx::core::Ticks{static_cast<std::int64_t>(i)});
    }
    CHECK(rig.engine->backlog() > 0);
    static_cast<void>(rig.render(64));
    rig.engine->pump();
    CHECK(rig.engine->backlog() == 0);
    static_cast<void>(rig.render(64));
    CHECK(rig.engine->positionTicks() ==
          adx::core::Ticks{static_cast<std::int64_t>(adx::graph::EngineCore::kQueueCapacity + 9)});
    adx::rt::ViolationLog::instance().reset();
}
