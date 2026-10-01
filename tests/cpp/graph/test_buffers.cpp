// Buffer planning, and the end of the 16-bus ceiling (FINAL_PLAN §3.3.5).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

#include "engine/graph/BufferPool.h"
#include "engine/graph/GraphBuilder.h"
#include "engine/graph/nodes/InsertNode.h"
#include "engine/graph/nodes/MeterNode.h"
#include "engine/project/Project.h"
#include "engine/project/SnapshotBuilder.h"
#include "tests/cpp/graph/GraphFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

TEST_CASE("buffers_share_when_lifetimes_do_not_overlap", "[graph][buffers]") {
    const std::vector<adx::graph::LiveInterval> intervals{{.first = 0, .last = 1},
                                                          {.first = 1, .last = 2},
                                                          {.first = 2, .last = 3},
                                                          {.first = 3, .last = 3},
                                                          {.first = 4, .last = 9}};
    std::vector<std::uint32_t> assignment(intervals.size());
    std::vector<std::uint32_t> busy(intervals.size());
    const std::uint32_t used = adx::graph::assignBuffers(intervals, assignment, busy);

    // [0,1] and [1,2] overlap at step 1, so they differ; [0,1] and [2,3] do not.
    CHECK(assignment[0] != assignment[1]);
    CHECK(assignment[2] == assignment[0]);
    CHECK(assignment[3] != assignment[2]);
    CHECK(used == 2);
}

TEST_CASE("bus_count_dynamic", "[graph][buffers]") {
    // 200 channels, each on its own insert. Iteration one had 16 buses and folded
    // track 17 onto bus 16 without a word. Here every insert gets a strip, and each
    // strip's meter reads only what went through it.
    adx::tests::SyntheticSpec spec;
    spec.channels = 200;
    spec.notes = 200; // one note per channel
    spec.length = adx::core::Ticks{adx::core::kPpq};
    spec.maxNoteTicks = adx::core::kPpq / 2;
    adx::project::Project project = adx::tests::syntheticProject(spec);
    // Every note identical, so every channel produces the same signal, and give every
    // insert a different gain: then insert k's meter reads exactly gain_k times one
    // channel's peak, and any folding shows up as a wrong ratio.
    for (auto& pattern : project.patterns) {
        for (auto& note : pattern.noteClips.front().notes) {
            note.start = adx::core::Ticks{0};
            note.length = adx::core::Ticks{adx::core::kPpq / 2};
            note.pitch = 60;
            note.velocity = 100;
        }
    }
    for (std::size_t i = 1; i < project.mixer.inserts.size(); ++i) {
        project.mixer.inserts[i].gain = static_cast<float>(i) / 256.0F;
    }

    adx::render::RenderStats stats;
    adx::render::OfflineRenderOptions options;
    options.frames = 4800;
    options.blockFrames = 256;

    // Rendered through an engine we can inspect afterwards.
    adx::graph::NodeStore nodes{adx::graph::PrepareInfo{}};
    const adx::graph::GraphBuild build = adx::graph::buildGraph(project, nodes);
    REQUIRE(build.ok);
    std::set<const adx::graph::Node*> faders;
    for (const auto& node : build.graph.nodes()) {
        if (node.label.starts_with("insert.") && node.paramCount == adx::graph::kInsertParamCount) {
            faders.insert(node.node.get());
        }
    }
    CHECK(faders.size() == 201);
    const adx::graph::CompiledGraph compiled{build.graph, build.order, build.pdc};
    // Distinct strips without distinct memory: the plan shares buffers whose
    // lifetimes do not overlap, so 200 strips need far fewer than 200 x 3 buffers.
    INFO("buffers planned: " << compiled.bufferCount());
    CHECK(compiled.bufferCount() < 16);

    const std::vector<float> out = adx::tests::renderFrames(project, 256, 4800, stats);
    REQUIRE(stats.peak > 0.0F);

    // Render again through a builder whose meters we can read.
    adx::project::SnapshotBuilder builder{48000, 2048};
    auto result = builder.build(project, 1, adx::project::dirty::kAll);
    REQUIRE(result.snapshot != nullptr);
    adx::tests::ManualRender rig; // for its arena and transport
    rig.transport.arrangement().bindTempo(result.snapshot->tempo, 48000);
    rig.transport.arrangement().requestState(adx::transport::PlayState::Playing);
    std::vector<float> block(std::size_t{4800} * 2);
    for (std::uint32_t done = 0; done < 4800; done += 240) {
        rig.arena.reset();
        rig.scheduler.render(*result.snapshot, rig.transport, rig.arena,
                             block.data() + (std::size_t{done} * 2), 240, 2);
    }

    std::vector<float> peaks;
    for (std::size_t i = 1; i < project.mixer.inserts.size(); ++i) {
        const auto meter = builder.nodes().findMeter(project.mixer.inserts[i].id);
        REQUIRE(meter != nullptr);
        std::array<adx::rt::LevelFrame, 32> frames{};
        const std::size_t got = meter->ring().readLatest(frames.data(), frames.size());
        REQUIRE(got > 0);
        float peak = 0.0F;
        for (std::size_t f = 0; f < got; ++f) {
            peak = std::max(peak, frames[f].peakLeft);
        }
        peaks.push_back(peak);
    }
    // peak_k / gain_k is the same for every strip, and no two strips read the same.
    const float unit = peaks[0] / project.mixer.inserts[1].gain;
    REQUIRE(unit > 0.0F);
    for (std::size_t k = 0; k < peaks.size(); ++k) {
        const float expected = unit * project.mixer.inserts[k + 1].gain;
        CHECK(std::abs(peaks[k] - expected) <= expected * 1e-5F);
    }
    CHECK(peaks[16] != peaks[15]); // insert 17 (index 16 of the buses) is not bus 16
    adx::project::destroySnapshot(result.snapshot);
}
