// Phase 6's engine work, rendered: controllers driving parameters, the project's tuning
// reaching instruments (P4-3), a send that ramps (P3-3), audition notes (P5-1), the
// metronome (P5-3), and a slot reorder that keeps every effect's node.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <numeric>
#include <set>
#include <vector>

#include "engine/graph/nodes/SendNode.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/SnapshotBuilder.h"
#include "engine/project/commands/ArrangeCommands.h"
#include "engine/rt/BlockArena.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::core::Ticks;

namespace {

/// RMS of the left channel of interleaved stereo over frames [from, to).
double rms(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t frame = from; frame < to; ++frame) {
        sum += static_cast<double>(samples[frame * 2]) * samples[frame * 2];
    }
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, to - from)));
}

float peak(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    float best = 0.0F;
    for (std::size_t frame = from; frame < to && frame * 2 < samples.size(); ++frame) {
        best = std::max(best, std::abs(samples[frame * 2]));
    }
    return best;
}

/// Pearson correlation of two equal-length series.
double correlation(const std::vector<double>& a, const std::vector<double>& b) {
    const auto n = static_cast<double>(a.size());
    const double meanA = std::accumulate(a.begin(), a.end(), 0.0) / n;
    const double meanB = std::accumulate(b.begin(), b.end(), 0.0) / n;
    double cov = 0.0;
    double varA = 0.0;
    double varB = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        cov += (a[i] - meanA) * (b[i] - meanB);
        varA += (a[i] - meanA) * (a[i] - meanA);
        varB += (b[i] - meanB) * (b[i] - meanB);
    }
    return cov / std::sqrt(varA * varB);
}

constexpr const char* kToneProject = R"([PROJECT]
ADX_VERSION=2

[CHANNEL Tone]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN P]
LENGTH=8:0:0
NOTES Tone
  A4 0:0:0 8:0:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
)";

} // namespace

TEST_CASE("controller_lfo_drives_param", "[render][controllers][phase6]") {
    // An LFO on the tone's volume: the output's envelope follows the LFO's sine
    // (phase_6.md §5). 120 bpm, one cycle a bar: two seconds.
    auto loaded = adx::tests::loadText(std::string(kToneProject) + R"(
[CONTROLLER Swell]
TYPE=lfo
TARGET=channel.Tone.volume
PERIOD=1:0:0
DEPTH=0.25
CENTER=0.25
)");
    REQUIRE(loaded->project.controllers.size() == 1);
    adx::render::RenderStats stats;
    const std::vector<float> out = adx::render::renderOffline(
        loaded->project, adx::render::OfflineRenderOptions{.frames = std::uint64_t{48000} * 4},
        stats);

    constexpr std::size_t kWindow = 960; // 20 ms
    std::vector<double> measured;
    std::vector<double> expected;
    for (std::size_t from = 0; from + kWindow <= std::size_t{48000} * 4; from += kWindow) {
        measured.push_back(rms(out, from, from + kWindow));
        const double seconds = (static_cast<double>(from) + (kWindow / 2.0)) / 48000.0;
        // volume = 2 * (0.25 + 0.25 sin(2 pi t / 2 s)): 0 .. 1
        expected.push_back(2.0 *
                           (0.25 + (0.25 * std::sin(2.0 * std::numbers::pi * seconds / 2.0))));
    }
    const double r = correlation(measured, expected);
    INFO("correlation " << r);
    CHECK(r > 0.99);
    const auto [low, high] = std::ranges::minmax(measured);
    CHECK(low < high * 0.05);
}

TEST_CASE("controller_peak_follows_its_source", "[render][controllers][phase6]") {
    // The Lead's volume follows the Key insert's level: loud while the key plays, silent
    // once it stops.
    auto loaded = adx::tests::loadText(R"([PROJECT]
ADX_VERSION=2

[CHANNEL Key]
INSTRUMENT=testtone
OUTPUT=insert.2

[CHANNEL Lead]
INSTRUMENT=testtone
OUTPUT=insert.3

[PATTERN P]
LENGTH=2:0:0
NOTES Key
  A4 0:0:0 0:2:0 100
NOTES Lead
  E5 0:0:0 2:0:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
INSERT 2 name="Key" gain=0
INSERT 3 name="Lead"
ROUTE insert.2 -> insert.1
ROUTE insert.3 -> insert.1

[CONTROLLER Follow]
TYPE=peak
TARGET=channel.Lead.volume
SOURCE=insert.2
CENTER=0
DEPTH=1
GAIN=24
ATTACK=1
RELEASE=20
)");
    // The key insert's fader is at zero so only the Lead is heard - but the follower reads
    // the key *post*-fader, so it hears nothing either. Turn the fader up and mute the
    // key's route instead: the follower still hears it.
    loaded->project.mixer.find(adx::core::InsertId{2})->gain = 1.0F;
    loaded->project.mixer.routes.erase(loaded->project.mixer.routes.begin());
    {
        // Structure first: the controller is a node, fed from the key's fader, writing the
        // Lead's volume.
        adx::project::SnapshotBuilder builder{48000, 1024};
        adx::project::Snapshot* snapshot =
            builder.build(loaded->project, 1, adx::project::dirty::kAll).snapshot;
        REQUIRE(snapshot != nullptr);
        const auto& nodes = builder.graphBuild()->graph.nodes();
        const auto controller = std::ranges::find(nodes, std::string("controller.Follow"),
                                                  &adx::graph::GraphNode::label);
        REQUIRE(controller != nodes.end());
        CHECK(controller->modulates != adx::graph::kNone);
        const auto step = std::ranges::find(snapshot->graph.steps, controller->node.get(),
                                            &adx::graph::NodeStep::node);
        REQUIRE(step != snapshot->graph.steps.end());
        CHECK(step->modulates == controller->modulates);
        adx::project::destroySnapshot(snapshot);
    }
    adx::render::RenderStats stats;
    const std::vector<float> out = adx::render::renderOffline(
        loaded->project, adx::render::OfflineRenderOptions{.frames = std::uint64_t{48000} * 2},
        stats);
    const double during = rms(out, 4800, 43200); // the key plays 0 .. 1 s (two beats)
    const double after = rms(out, 60000, 96000); // and has stopped by 1.25 s
    INFO("during " << during << ", after " << after);
    CHECK(during > 0.05);
    CHECK(after < during * 0.02);
}

TEST_CASE("tuning_reaches_instruments_at_432", "[render][phase6]") {
    // P4-3: TUNING=432 moves A4 to 432 Hz. Counted by rising zero crossings over a second.
    std::string text = kToneProject;
    text.replace(text.find("ADX_VERSION=2"), 13, "ADX_VERSION=2\nTUNING=432");
    auto loaded = adx::tests::loadText(text);
    REQUIRE(loaded->project.meta.tuning == 432.0);
    adx::render::RenderStats stats;
    const std::vector<float> out = adx::render::renderOffline(
        loaded->project, adx::render::OfflineRenderOptions{.frames = std::uint64_t{48000} * 2},
        stats);
    int crossings = 0;
    for (std::size_t frame = 48000; frame < 96000; ++frame) {
        crossings += out[(frame - 1) * 2] < 0.0F && out[frame * 2] >= 0.0F ? 1 : 0;
    }
    CHECK(std::abs(crossings - 432) <= 1);

    // And 440 is untouched: the same project at standard pitch.
    auto standard = adx::tests::loadText(kToneProject);
    const std::vector<float> reference = adx::render::renderOffline(
        standard->project, adx::render::OfflineRenderOptions{.frames = std::uint64_t{48000} * 2},
        stats);
    crossings = 0;
    for (std::size_t frame = 48000; frame < 96000; ++frame) {
        crossings += reference[(frame - 1) * 2] < 0.0F && reference[frame * 2] >= 0.0F ? 1 : 0;
    }
    CHECK(std::abs(crossings - 440) <= 1);
}

TEST_CASE("send_level_ramps_across_a_block", "[render][phase6]") {
    // P3-3: a send's level moved by a knob ramps from the old value to the new one over
    // the next block, instead of jumping at its first frame.
    adx::graph::SendNode node;
    node.prepare(adx::graph::PrepareInfo{});
    std::vector<std::byte> storage(1U << 16U);
    adx::rt::BlockArena arena{storage.data(), storage.size()};
    const adx::transport::TimeSource time;
    constexpr std::uint32_t kFrames = 64;
    const std::vector<float> ones(kFrames, 1.0F);
    std::vector<float> left(kFrames);
    std::vector<float> right(kFrames);
    const std::array<std::span<const float>, 2> inputs{std::span<const float>{ones},
                                                       std::span<const float>{ones}};
    const std::array<std::span<float>, 2> outputs{std::span<float>{left}, std::span<float>{right}};
    const auto block = [&](float level) {
        const std::array<float, 1> params{level};
        adx::graph::ProcessContext context{.time = time,
                                           .outputs = outputs,
                                           .inputs = inputs,
                                           .frames = kFrames,
                                           .sampleRate = 48000,
                                           .events = {},
                                           .params = params,
                                           .automation = {},
                                           .arena = arena};
        node.process(context);
    };
    block(0.5F);
    CHECK(std::ranges::all_of(left, [](float v) { return v == 0.5F; }));
    block(1.0F);
    CHECK(left.front() > 0.5F);
    CHECK(left.front() < 0.6F);
    CHECK(left.back() == 1.0F);
    CHECK(std::ranges::is_sorted(left));
    block(1.0F);
    CHECK(std::ranges::all_of(left, [](float v) { return v == 1.0F; }));
}

TEST_CASE("audition_sounds_without_the_transport", "[render][phase6]") {
    // P5-1: a drawn note sounds once, through its channel's instrument, with the
    // transport stopped and no snapshot rebuild.
    const adx::tests::OfflineRig rig{256};
    const adx::project::Project project =
        adx::tests::toneProject({{.start = kPpq * 64, .length = kPpq, .pitch = 60}});
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    CHECK(peak(rig.render(4800), 0, 4800) == 0.0F);

    const std::uint64_t built = rig.engine->snapshotsBuilt();
    rig.engine->audition(project.channels.front().id, 69, 100, 0.25);
    const std::vector<float> heard = rig.render(48000);
    CHECK(peak(heard, 0, 9600) > 0.05F);      // sounding in the first 200 ms
    CHECK(peak(heard, 36000, 48000) == 0.0F); // released and finished by 0.75 s
    CHECK(rig.engine->snapshotsBuilt() == built);

    // A transport stop does not cut it: it is not the arrangement's.
    rig.engine->audition(project.channels.front().id, 72, 100, 1.0);
    static_cast<void>(rig.render(2400));
    rig.engine->stopPlayback();
    CHECK(peak(rig.render(4800), 0, 4800) > 0.05F);
    rig.engine->stopAudition();
    const std::vector<float> stopped = rig.render(48000);
    CHECK(peak(stopped, 36000, 48000) == 0.0F);
}

TEST_CASE("metronome_clicks_on_the_beat_when_enabled", "[render][phase6]") {
    // P5-3: the click sounds on every beat while rolling, and nothing else does - the
    // project has no notes.
    const adx::tests::OfflineRig rig{256};
    const adx::project::Project project = adx::tests::toneProject({});
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    rig.engine->play();
    CHECK(peak(rig.render(48000), 0, 48000) == 0.0F);

    rig.engine->stopPlayback();
    rig.engine->seek(Ticks{0});
    rig.engine->setMetronome(true, Ticks{kPpq}, 4);
    rig.engine->play();
    const std::vector<float> clicks = rig.render(48000); // 120 bpm: beats at 0 and 0.5 s
    CHECK(peak(clicks, 0, 1000) > 0.1F);
    CHECK(peak(clicks, 2400, 23000) == 0.0F);
    CHECK(peak(clicks, 24000, 25000) > 0.1F);

    rig.engine->setMetronome(false, Ticks{kPpq}, 4);
    CHECK(peak(rig.render(48000), 0, 48000) == 0.0F);
}

TEST_CASE("mixer_slot_reorder_keeps_nodes", "[render][phase6]") {
    // Reordering effects is one command and keeps every slot's node - and with it the
    // effect's state - because a slot's id, not its position, is its identity.
    auto loaded = adx::tests::loadText(
        std::string(kToneProject)
            .replace(std::string(kToneProject).find("INSERT 1 name=\"Master\""), 22,
                     "INSERT 1 name=\"Master\"\n  SLOT 1 EQ\n  SLOT 2 Delay\n  SLOT 3 Reverb"));
    adx::project::SnapshotBuilder builder{48000, 1024};
    adx::project::Snapshot* first =
        builder.build(loaded->project, 1, adx::project::dirty::kAll).snapshot;
    REQUIRE(first != nullptr);
    std::set<const adx::graph::Node*> before;
    // Graph nodes in the order they were added: the channel, then the master's slots in
    // list order (GraphBuilder.h).
    std::vector<const adx::graph::Node*> beforeOrder;
    for (const auto& node : builder.graphBuild()->graph.nodes()) {
        before.insert(node.node.get());
        beforeOrder.push_back(node.node.get());
    }
    const std::uint64_t created = builder.nodes().created();

    const adx::core::SlotId reverb = loaded->project.master()->slots[2].id;
    const std::size_t depth = loaded->stack.undoDepth();
    loaded->stack.execute(std::make_unique<adx::project::MoveSlot>(reverb, 0), loaded->project);
    CHECK(loaded->stack.undoDepth() == depth + 1);
    adx::project::Snapshot* second =
        builder.build(loaded->project, 2, adx::project::dirty::kMixer).snapshot;
    REQUIRE(second != nullptr);
    std::set<const adx::graph::Node*> after;
    for (const auto& node : builder.graphBuild()->graph.nodes()) {
        after.insert(node.node.get());
    }
    CHECK(after == before);
    CHECK(builder.nodes().created() == created);
    // The order changed: the reverb's node - the third slot's before the move - now runs
    // before the EQ's, which ran first.
    const auto& steps = second->graph.steps;
    const auto position = [&steps](const adx::graph::Node* node) {
        return std::ranges::find(steps, node, &adx::graph::NodeStep::node) - steps.begin();
    };
    CHECK(position(beforeOrder[3]) < position(beforeOrder[1]));
    CHECK(position(beforeOrder[1]) < position(beforeOrder[2]));
    adx::project::destroySnapshot(first);
    adx::project::destroySnapshot(second);
}
