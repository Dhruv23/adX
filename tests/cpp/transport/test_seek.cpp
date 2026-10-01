// Seek semantics (phase_3.md §4.11): release, never cut; never leave a note stuck.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

#include "engine/graph/nodes/ChannelNode.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::core::Ticks;
using adx::tests::NoteSpec;
using adx::tests::OfflineRig;

namespace {

float largestStep(const std::vector<float>& out) {
    float largest = 0.0F;
    for (std::size_t frame = 1; frame < out.size() / 2; ++frame) {
        largest = std::max(largest, std::abs(out[frame * 2] - out[(frame - 1) * 2]));
    }
    return largest;
}

std::uint32_t soundingVoices(OfflineRig& rig) {
    std::uint32_t sounding = 0;
    const adx::graph::CompiledGraph* graph = rig.engine->builder().compiledGraph();
    REQUIRE(graph != nullptr);
    for (const adx::graph::NodeStep& step : graph->steps()) {
        if (auto* channel = dynamic_cast<adx::graph::ChannelNode*>(step.node)) {
            for (const adx::graph::Voice& voice : channel->pool().voices()) {
                sounding += voice.phase != adx::graph::VoicePhase::Free ? 1U : 0U;
            }
        }
    }
    return sounding;
}

} // namespace

TEST_CASE("seek_releases_not_cuts", "[transport][seek]") {
    // One long, low note. Seek into the middle of it while it plays: the voice must
    // decay over its release - no jump - and must not start again, because its
    // note-on is behind the new position.
    const adx::project::Project project =
        adx::tests::toneProject({NoteSpec{.start = 0, .length = kPpq * 8, .pitch = 36}});
    const OfflineRig rig{256};
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    rig.engine->play();

    std::vector<float> out;
    rig.render(24000, out);
    const float before = std::abs(out[std::size_t{24000 - 1} * 2]);
    REQUIRE(before > 0.01F);

    rig.engine->seek(Ticks{kPpq * 4});
    rig.render(24000, out);

    CHECK(largestStep(out) < 0.01F);
    // Still sounding just after the seek: released, not cut.
    CHECK(std::abs(out[std::size_t{24000 + 10} * 2]) > 0.0F);
    // Silent once the 50 ms release is over, and it stays silent.
    for (std::size_t frame = 24000 + 2400 + 256; frame < 48000; ++frame) {
        REQUIRE(out[frame * 2] == 0.0F);
    }
}

TEST_CASE("seek_no_stuck_notes", "[transport][seek]") {
    // A thousand seeks, at random positions, at random moments, while eight busy
    // channels play. Stop, let the releases finish, and not one voice may remain.
    adx::tests::SyntheticSpec spec;
    spec.channels = 8;
    spec.notes = 4000;
    spec.maxNoteTicks = kPpq * 2;
    const adx::project::Project project = adx::tests::syntheticProject(spec);

    OfflineRig rig{256};
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    rig.engine->play();

    std::mt19937 random{2026};
    std::vector<float> scratch;
    for (int seek = 0; seek < 1000; ++seek) {
        scratch.clear();
        rig.render(64 + (random() % 2048), scratch);
        rig.engine->seek(Ticks{
            static_cast<std::int64_t>(random() % static_cast<std::uint32_t>(spec.length.value))});
    }
    rig.render(4800, scratch);
    REQUIRE(soundingVoices(rig) > 0); // it really was busy

    rig.engine->stopPlayback();
    scratch.clear();
    rig.render(48000, scratch);
    CHECK(soundingVoices(rig) == 0);
    for (std::size_t frame = 24000; frame < 48000; ++frame) {
        REQUIRE(scratch[frame * 2] == 0.0F);
    }
}
