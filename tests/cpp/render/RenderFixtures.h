// Projects to render, and the two ways of rendering them.
//
// Shared by the graph, transport and render suites. Projects are built directly on the
// model rather than through .adx text where the test is about scale (100k notes parse
// in ~150 ms, which would dominate a test that runs at three block sizes), and through
// text where the test is about a specific, readable arrangement.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/OfflineBackend.h"
#include "engine/format/adx/Diagnostics.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/render/OfflineRender.h"
#include "engine/render/RenderEngine.h"

namespace adx::tests {

/// A project loaded from text, with the stack that built it.
struct Loaded {
    project::Project project;
    project::CommandStack stack;
    format::DiagnosticList diagnostics;
};

/// Parses v2 text. Fails the calling test (REQUIRE) on any error diagnostic.
std::unique_ptr<Loaded> loadText(std::string_view text);

/// The shape of a synthetic project: `channels` channels, each on its own insert
/// routed to the master, each with its own pattern placed once at tick 0, and
/// `notes` notes spread across `length` ticks. Deterministic in `seed`.
struct SyntheticSpec {
    std::uint32_t channels{200};
    std::uint32_t notes{100'000};
    core::Ticks length{core::kPpq * 4 * 16};
    std::uint32_t seed{1};
    double bpm{128.0};
    /// Longest note, in ticks. Short notes keep the voice count sane at 200 channels.
    std::int64_t maxNoteTicks{core::kPpq / 2};
};

[[nodiscard]] project::Project syntheticProject(const SyntheticSpec& spec);

/// One note for toneProject.
struct NoteSpec {
    std::int64_t start{0};
    std::int64_t length{core::kPpq / 8};
    std::uint8_t pitch{69};
    std::uint8_t velocity{100};
};

/// One channel on the master, one pattern at tick 0 holding `notes`, at `bpm`.
[[nodiscard]] project::Project toneProject(const std::vector<NoteSpec>& notes, double bpm = 120.0,
                                           std::uint16_t polyphony = 16);

/// Plays `project` from tick 0 through a RenderEngine on a NullBackend - a real audio
/// thread on a real clock - and captures `frames` of what the callback produced.
struct RealtimeCapture {
    std::vector<float> samples;
    std::uint64_t violations{0};
    std::size_t arenaHighWater{0};
    std::size_t arenaCapacity{0};
    std::uint64_t worstCallbackNs{0};
};

[[nodiscard]] RealtimeCapture captureRealtime(const project::Project& project,
                                              std::uint32_t blockFrames, std::uint64_t frames,
                                              std::uint32_t sampleRate = 48000);

/// The same, offline.
[[nodiscard]] std::vector<float> renderFrames(const project::Project& project,
                                              std::uint32_t blockFrames, std::uint64_t frames,
                                              render::RenderStats& stats,
                                              std::uint32_t sampleRate = 48000);

/// A RenderEngine on an OfflineBackend, kept alive so a test can render in pieces,
/// post transport changes between them, and read the scheduler's counters.
struct OfflineRig {
    audio::OfflineBackend* backend{nullptr};
    std::unique_ptr<render::RenderEngine> engine;

    explicit OfflineRig(std::uint32_t blockFrames, std::uint32_t sampleRate = 48000);

    /// Renders the next `frames` and appends them to `out`.
    void render(std::uint64_t frames, std::vector<float>& out) const;
    [[nodiscard]] std::vector<float> render(std::uint64_t frames) const;

    [[nodiscard]] const graph::SchedulerStats& stats() {
        return engine->core().scheduler().stats();
    }
};

/// Index of the first frame at or after `from` whose left sample is non-zero, or
/// `samples.size() / channels` when there is none.
[[nodiscard]] std::size_t firstSoundFrom(const std::vector<float>& samples, std::size_t from,
                                         std::uint32_t channels = 2);

/// True when optimisations are on - the configurations where timing budgets mean
/// anything.
[[nodiscard]] constexpr bool optimisedBuild() noexcept {
#if defined(NDEBUG)
    return true;
#else
    return false;
#endif
}

} // namespace adx::tests
