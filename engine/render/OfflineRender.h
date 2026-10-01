// Offline render: the same engine, driven by OfflineBackend instead of a clock.
//
// There is no separate offline renderer. This builds a RenderEngine around an
// OfflineBackend and pulls frames through the identical AudioThread -> CallbackCore ->
// EngineCore -> Scheduler -> graph path a live stream uses - the architecture
// FINAL_PLAN §3.1 credits the archived ExportRenderer with, kept (phase_3.md §4.10).
// The block size is a parameter, not a fixed 4096, so an export can use exactly the
// block size the realtime stream did.
//
// File formats are Phase 8's. writeWavFloat32 exists so Phase 3 can put a render on
// disk and listen to it; it is the simplest format there is, and nothing more.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Time.h"
#include "engine/render/RenderHash.h"

namespace adx::project {
class Project;
}

namespace adx::render {

struct OfflineRenderOptions {
    std::uint32_t sampleRate{48000};
    std::uint32_t blockFrames{256};
    std::uint32_t channels{2};
    core::Ticks start{0};
    /// At or before `start` means "to the end of the arrangement".
    core::Ticks end{0};
    /// Rendered past `end`, so releases finish rather than being cut off.
    double tailSeconds{0.0};
    /// When non-zero, render exactly this many frames from `start` and ignore `end`
    /// and `tailSeconds`. What a test comparing two renders sample for sample wants.
    std::uint64_t frames{0};
};

struct RenderStats {
    std::uint64_t frames{0};
    std::uint32_t sampleRate{0};
    std::uint32_t blockFrames{0};
    std::uint32_t channels{0};
    std::uint64_t callbacks{0};
    /// Realtime violations recorded during the render. Zero, or the graph allocated.
    /// Meaningful only where the allocator hook is compiled in.
    std::uint64_t violations{0};
    float peak{0.0F};
    double rms{0.0};
    std::uint32_t latencySamples{0};
    RenderHash hash;
    std::size_t arenaHighWater{0};
    std::size_t arenaCapacity{0};
    double wallSeconds{0.0};
    /// Non-empty when the project could not be rendered (a routing cycle).
    std::string error;
};

/// Renders `project` and returns the interleaved result.
[[nodiscard]] std::vector<float> renderOffline(const project::Project& project,
                                               const OfflineRenderOptions& options,
                                               RenderStats& stats);

/// Peak, RMS and hash of an interleaved buffer, filled into `stats`.
void measure(std::span<const float> samples, RenderStats& stats) noexcept;

/// 32-bit float WAV (WAVE_FORMAT_IEEE_FLOAT). False if the file could not be written.
bool writeWavFloat32(const std::string& path, std::span<const float> samples,
                     std::uint32_t channels, std::uint32_t sampleRate);

} // namespace adx::render
