#include "tests/cpp/render/RenderEvidence.h"

#include <algorithm>

#include "tests/cpp/Env.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace adx::tests {

const RenderEvidence& loadEvidence() {
    static const RenderEvidence kEvidence = [] {
        RenderEvidence out;
        const SyntheticSpec spec; // 200 channels, 100k notes, 16 bars at 128 bpm
        const project::Project project = syntheticProject(spec);

        // The whole arrangement plus a quarter second of release in optimised builds.
        // Debug renders the first four seconds - ~13k of the notes - because a Debug
        // build of 400 nodes runs at a fraction of real time, and the realtime half of
        // this test cannot run faster than the clock anyway. The nightly job sets
        // ADX_FULL_EVIDENCE and renders the whole thing in Debug too (phase_3.md §10).
        const std::int64_t whole =
            project.tempo.toSamples(project.contentLength(), 48000).value + 12000;
        const bool full = optimisedBuild() || environment("ADX_FULL_EVIDENCE").has_value();
        const auto frames = static_cast<std::uint64_t>(full ? whole : std::int64_t{48000} * 4);
        out.description = std::string{"200 channels, 100000 notes, "} + std::to_string(frames) +
                          " frames per render (" +
                          (full ? "the whole project" : "Debug: the first 4 s") + ")";

        for (const std::uint32_t block : {64U, 256U, 1024U}) {
            RenderRun run;
            run.blockFrames = block;
            run.frames = frames;

            render::RenderStats stats;
            const std::vector<float> offline = renderFrames(project, block, frames, stats);
            run.offline = render::hashSamples(offline);
            run.offlinePeak = stats.peak;
            run.offlineViolations = stats.violations;

            const RealtimeCapture captured = captureRealtime(project, block, frames);
            run.realtime = render::hashSamples(captured.samples);
            run.realtimeViolations = captured.violations;
            run.arenaHighWater = std::max(stats.arenaHighWater, captured.arenaHighWater);
            run.arenaCapacity = captured.arenaCapacity;
            out.runs.push_back(run);
        }
        return out;
    }();
    return kEvidence;
}

} // namespace adx::tests
