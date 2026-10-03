// Drives one effect node directly - no graph, no scheduler - for the effect tests.
//
// Everything the run needs is allocated before the first process() call, and every
// process() call runs inside a ScopedRtSection, so any effect test is also an
// allocation test: `violations` counts what the allocator hook saw (phase_4.md §6,
// instrument_no_alloc_in_process's effect counterpart).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "engine/graph/nodes/SlotNode.h"

namespace adx::tests {

inline constexpr std::uint32_t kTestRate = 48000;

/// A slot's full parameter slice for `type`: mix, bypass, then every descriptor of the
/// type at its default, in table order - what the graph builder would hand the node.
[[nodiscard]] std::vector<float> slotParams(std::string_view type, float mix = 1.0F,
                                            bool bypass = false);

/// Index into a slot's parameter slice of the effect parameter `name`.
[[nodiscard]] std::uint32_t slotParamIndex(std::string_view type, std::string_view name);

struct StereoSignal {
    std::vector<float> left;
    std::vector<float> right;
    [[nodiscard]] std::size_t size() const noexcept {
        return left.size();
    }
};

[[nodiscard]] StereoSignal sine(double frequency, float amplitude, std::size_t frames);
[[nodiscard]] StereoSignal noise(std::uint32_t seed, float amplitude, std::size_t frames);
[[nodiscard]] StereoSignal impulse(std::size_t at, float amplitude, std::size_t frames);

struct EffectRun {
    StereoSignal out;
    /// Allocation and deallocation violations recorded during process() calls.
    std::uint64_t violations{0};
};

/// Called before each block with the block's first frame; may change `params`.
using ParamSchedule = std::function<void(std::uint64_t frame, std::vector<float>& params)>;

/// Runs `node` (already prepared at kTestRate) over `in` in blocks of `block` frames.
EffectRun runEffect(graph::SlotNode& node, const StereoSignal& in, std::vector<float> params,
                    std::uint32_t block = 64, const ParamSchedule& schedule = {},
                    const StereoSignal* side = nullptr);

/// Prepares a fresh node of `type` at kTestRate.
[[nodiscard]] std::shared_ptr<graph::SlotNode> preparedEffect(std::string_view type);

} // namespace adx::tests
