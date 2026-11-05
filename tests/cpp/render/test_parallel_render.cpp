// The parallel scheduler (RenderGraph.h ParallelPlan, Crew.h): a block spread over
// helper threads renders the same bits as one thread walking the steps in order.
//
// That is the whole contract. The golden hashes are taken with whatever the default
// worker count is on the machine running the suite, so they already cover it from one
// side; this compares the two paths directly, on projects that exercise each kind of
// edge the plan has to keep in order - sends, sidechains, PDC delay lines, automation,
// and many channels on many inserts.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "engine/format/adx/Parser.h"
#include "engine/graph/Crew.h"
#include "engine/render/RenderHash.h"
#include "engine/render/RenderWorkers.h"
#include "tests/cpp/render/GoldenHashes.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

struct Rendered {
    std::string hash;
    std::uint64_t parallelPieces{0};
    float peak{0.0F};
};

Rendered renderWith(const adx::project::Project& project, std::int32_t workers,
                    std::uint32_t blockFrames, std::uint64_t frames) {
    adx::render::OfflineRenderOptions options;
    options.blockFrames = blockFrames;
    options.frames = frames;
    options.renderWorkers = workers;
    adx::render::RenderStats stats;
    static_cast<void>(adx::render::renderOffline(project, options, stats));
    REQUIRE(stats.error.empty());
    return Rendered{
        .hash = stats.hash.hex(), .parallelPieces = stats.parallelPieces, .peak = stats.peak};
}

} // namespace

TEST_CASE("parallel_render_matches_serial_synthetic", "[render][parallel]") {
    // 64 channels on 64 inserts: enough steps that every piece goes to the crew.
    adx::tests::SyntheticSpec spec;
    spec.channels = 64;
    spec.notes = 20'000;
    const adx::project::Project project = adx::tests::syntheticProject(spec);
    for (const std::uint32_t block : {64U, 256U, 1024U}) {
        INFO("block " << block);
        const Rendered serial = renderWith(project, 0, block, std::uint64_t{48000} * 4);
        const Rendered parallel = renderWith(project, 3, block, std::uint64_t{48000} * 4);
        CHECK(serial.parallelPieces == 0);
        CHECK(parallel.parallelPieces > 0);
        CHECK(serial.peak > 0.0F);
        CHECK(parallel.hash == serial.hash);
    }
}

TEST_CASE("parallel_render_matches_serial_golden", "[render][parallel]") {
    // Every golden project, which between them route through sends, sidechains, delay
    // compensation and automation lanes.
    int spread = 0;
    for (const auto& entry : std::filesystem::directory_iterator(adx::tests::goldenDirectory())) {
        if (entry.path().extension() != ".adx") {
            continue;
        }
        INFO(entry.path().filename().string());
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>()};
        adx::project::Project project;
        adx::project::CommandStack stack;
        adx::format::DiagnosticList diagnostics;
        adx::format::load(
            text, project, stack, diagnostics,
            adx::format::ParseOptions{.baseDirectory = entry.path().parent_path().string()});
        REQUIRE_FALSE(diagnostics.hasErrors());
        const Rendered serial = renderWith(project, 0, 256, std::uint64_t{48000} * 3);
        const Rendered parallel = renderWith(project, 4, 256, std::uint64_t{48000} * 3);
        CHECK(parallel.hash == serial.hash);
        if (parallel.parallelPieces > 0) {
            ++spread;
        }
    }
    // Small projects stay under the 16-step threshold and render serially either way;
    // the comparison only means something for the ones that reached the crew.
    CHECK(spread >= 3);
}

TEST_CASE("crew_runs_every_job_to_completion", "[graph][parallel]") {
    // The handshake on its own: thousands of jobs, each counting its work items off a
    // shared counter, must each see every item done exactly once before run() returns -
    // whichever helpers were awake for it.
    adx::render::RenderWorkers workers(3, false);
    adx::graph::Crew* crew = workers.crew();
    REQUIRE(crew != nullptr);
    REQUIRE(crew->lanes() == 4);

    struct Job {
        std::atomic<std::uint32_t> next{0};
        std::atomic<std::uint32_t> done{0};
        std::uint32_t items{0};
    };
    Job job;
    const auto task = [](void* user, std::uint32_t /*lane*/) noexcept {
        Job& j = *static_cast<Job*>(user);
        while (true) {
            const std::uint32_t item = j.next.fetch_add(1, std::memory_order_relaxed);
            if (item >= j.items) {
                return;
            }
            j.done.fetch_add(1, std::memory_order_relaxed);
        }
    };
    for (std::uint32_t round = 0; round < 5000; ++round) {
        job.next.store(0, std::memory_order_relaxed);
        job.done.store(0, std::memory_order_relaxed);
        job.items = 1 + (round % 97);
        crew->run(task, &job);
        REQUIRE(job.done.load(std::memory_order_relaxed) == job.items);
    }
    CHECK(crew->jobs() >= 5000);
}
