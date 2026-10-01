// The 200-channel / 100k-note renders the phase gate is judged on, computed once.
//
// Three test cases assert on them - bit identity, zero allocations, arena headroom -
// and each render costs real time: the realtime half runs on a real clock. So they
// are rendered the first time any of the three asks and shared after that, within the
// one test process that runs the [.slow] set.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/render/RenderHash.h"

namespace adx::tests {

struct RenderRun {
    std::uint32_t blockFrames{0};
    std::uint64_t frames{0};
    render::RenderHash offline;
    render::RenderHash realtime;
    float offlinePeak{0.0F};
    std::uint64_t offlineViolations{0};
    std::uint64_t realtimeViolations{0};
    /// The larger of the two paths' arena high-water marks, and the arena's size.
    std::size_t arenaHighWater{0};
    std::size_t arenaCapacity{0};
};

struct RenderEvidence {
    /// What was rendered, for the test log: an optimised build renders the whole
    /// project, a Debug build its first seconds.
    std::string description;
    std::vector<RenderRun> runs;
};

/// Renders the synthetic project offline and realtime at block sizes 64, 256 and 1024
/// the first time it is called.
[[nodiscard]] const RenderEvidence& loadEvidence();

} // namespace adx::tests
