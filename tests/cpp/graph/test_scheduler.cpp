// The scheduler: sample-accurate, split at tempo changes, and amortised.
//
// FINAL_PLAN §9: "The scheduler has sample-accuracy tests against hand-computed
// offsets." The test tone's first sample is never zero (it starts in cosine phase),
// so a note's onset is the first non-zero sample after silence, exactly.

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::tests::NoteSpec;
using adx::tests::OfflineRig;

namespace {

/// Where a tick lands at a constant tempo, written out rather than asked of the
/// TempoMap: seconds per tick is 60 / (bpm * PPQ), and the frame is rounded once.
std::int64_t handSamples(std::int64_t tick, double bpm, double rate = 48000.0) {
    const double secondsPerTick = 60.0 / (bpm * static_cast<double>(kPpq));
    return std::llround((static_cast<double>(tick) * secondsPerTick) * rate);
}

void playFromStart(OfflineRig& rig, const adx::project::Project& project) {
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    rig.engine->setLoop(adx::transport::LoopRegion{});
    rig.engine->seek(adx::core::Ticks{0});
    rig.engine->play();
}

} // namespace

TEST_CASE("scheduler_sample_accurate_offsets", "[graph][scheduler]") {
    // 200 notes at deliberately awkward ticks - on no grid, so a block boundary or a
    // rounding shortcut anywhere would show - spaced far enough apart that each note's
    // release has finished before the next begins.
    std::vector<NoteSpec> notes;
    std::int64_t tick = 0;
    for (int i = 0; i < 200; ++i) {
        tick += (kPpq / 2) + ((static_cast<std::int64_t>(i) * 1237) % (kPpq / 4));
        notes.push_back(NoteSpec{.start = tick, .length = kPpq / 16});
    }

    for (const double bpm : {120.0, 97.3, 174.0}) {
        const adx::project::Project project = adx::tests::toneProject(notes, bpm);
        const std::int64_t totalFrames = handSamples(tick + kPpq, bpm);
        for (const std::uint32_t block : {64U, 1024U}) {
            INFO("bpm " << bpm << ", block " << block);
            OfflineRig rig{block};
            playFromStart(rig, project);
            const std::vector<float> out = rig.render(static_cast<std::uint64_t>(totalFrames));

            for (const NoteSpec& note : notes) {
                const auto expected = static_cast<std::size_t>(handSamples(note.start, bpm));
                INFO("note at tick " << note.start << ", expected frame " << expected);
                REQUIRE(adx::tests::firstSoundFrom(out, expected - 1000) == expected);
            }
        }
    }
}

TEST_CASE("scheduler_tempo_change_midblock", "[graph][scheduler]") {
    // 120 bpm, then 90 from an odd tick that lands inside a 1024-frame block.
    const std::int64_t change = (4 * kPpq) + 100;
    const std::int64_t noteTick = change + (kPpq / 3);
    adx::project::Project project = adx::tests::toneProject({NoteSpec{.start = noteTick}}, 120.0);
    project.tempo.setTempo(adx::core::Ticks{change}, 90.0, false);

    const double changeSeconds = static_cast<double>(change) * 60.0 / (120.0 * kPpq);
    const double noteSeconds =
        changeSeconds + (static_cast<double>(noteTick - change) * 60.0 / (90.0 * kPpq));
    const auto changeFrame = std::llround(changeSeconds * 48000.0);
    const auto expected = static_cast<std::size_t>(std::llround(noteSeconds * 48000.0));
    REQUIRE(changeFrame % 1024 != 0); // the change really is mid-block

    OfflineRig rig{1024};
    playFromStart(rig, project);
    const std::uint64_t frames = expected + 4800;
    const std::vector<float> out = rig.render(frames);

    // The onset follows the new tempo exactly...
    CHECK(adx::tests::firstSoundFrom(out, 0) == expected);
    // ...because the block was split at the change: one more piece than blocks.
    const auto blocks = (frames + 1023) / 1024;
    CHECK(rig.stats().blocks == blocks);
    CHECK(rig.stats().subBlocks == blocks + 1);
}

TEST_CASE("scheduler_cursor_is_amortized", "[graph][scheduler]") {
    // Two projects with the same note density, one 100x longer than the other. Per
    // block, the scheduler must do the same event work in both: it walks a cursor
    // through the events that occur, and never looks at the rest. Iteration one scanned
    // every note in the project every callback (FINAL_PLAN §3.3.3), which would make
    // the long project's count 100x the short one's.
    const auto project = [](int notes) {
        std::vector<NoteSpec> specs;
        specs.reserve(static_cast<std::size_t>(notes));
        for (int i = 0; i < notes; ++i) {
            specs.push_back(NoteSpec{.start = (static_cast<std::int64_t>(i) * kPpq) / 8,
                                     .length = kPpq / 16,
                                     .pitch = static_cast<std::uint8_t>(48 + (i % 24))});
        }
        return adx::tests::toneProject(specs, 120.0, 64);
    };

    const auto measure = [&](int notes) {
        OfflineRig rig{256};
        playFromStart(rig, project(notes));
        static_cast<void>(rig.render(std::uint64_t{48000} * 5));
        return rig.stats();
    };

    const adx::graph::SchedulerStats small = measure(1'000);
    const adx::graph::SchedulerStats large = measure(100'000);
    INFO("comparisons: 1k notes " << small.eventComparisons << ", 100k notes "
                                  << large.eventComparisons);
    REQUIRE(small.eventComparisons > 0);
    CHECK(large.eventComparisons == small.eventComparisons);
    CHECK(large.eventsDispatched == small.eventsDispatched);
    // The one search, at the start, is logarithmic: 200k events is ~18 probes.
    CHECK(large.cursorSearches == small.cursorSearches);
    CHECK(large.searchProbes <= 20);
}
