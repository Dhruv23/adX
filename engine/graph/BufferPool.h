// The audio buffers a render graph runs on, and the plan that decides how few of them
// it needs.
//
// Iteration one had a fixed 16 buses because that is how many it allocated
// (`kMaxEngineTracks`, FINAL_PLAN §3.3.5). Here the count is a property of the graph:
// every port that carries audio gets a *live interval* - the steps between its first
// write and its last read - and intervals that do not overlap share a buffer. A
// 200-insert project therefore renders through 200 distinct strips without owning 200
// times the memory, and there is no constant anywhere that folds strip 17 onto 16.
//
// Deliberately not the per-callback BlockArena that phase_3.md §4 names. Buffers are
// planned once per graph and live as long as it does, which makes their size a fact
// about the project rather than a runtime high-water mark - and it leaves the arena's
// budget to the thing that genuinely varies per block, the events.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/graph/PortSpec.h"
#include "engine/rt/RtConfig.h"

namespace adx::graph {

/// Floats per planned buffer: one stereo port, full block.
inline constexpr std::size_t kFloatsPerBuffer =
    static_cast<std::size_t>(kPortChannels) * rt::kMaxBlockFrames;

/// When a buffer is in use, in step indices, inclusive at both ends.
struct LiveInterval {
    std::uint32_t first{0};
    std::uint32_t last{0};
};

/// Greedy interval colouring. Writes each interval's buffer index into `assignment`
/// and returns how many distinct buffers that needed. `busyUntil` is scratch with
/// room for as many buffers as there could be - intervals.size() is always enough.
///
/// Intervals must be sorted by `first`. With that, first-fit is optimal for interval
/// graphs, and the result is a pure function of the input, which is what keeps two
/// builds of the same project bit-identical.
std::uint32_t assignBuffers(std::span<const LiveInterval> intervals,
                            std::span<std::uint32_t> assignment,
                            std::span<std::uint32_t> busyUntil) noexcept;

/// The audio thread's view of the planned buffers.
class BufferPool {
public:
    BufferPool() noexcept = default;

    /// `storage` holds bufferCount * kFloatsPerBuffer floats and outlives the pool.
    BufferPool(std::span<float> storage, std::uint32_t bufferCount) noexcept
        : m_storage(storage), m_count(bufferCount) {}

    /// `frames` samples of one channel of one buffer, starting `offset` into the
    /// block.
    [[nodiscard]] std::span<float> channel(std::uint32_t buffer, std::uint32_t channelIndex,
                                           std::uint32_t offset,
                                           std::uint32_t frames) const noexcept;

    [[nodiscard]] std::uint32_t count() const noexcept {
        return m_count;
    }

private:
    std::span<float> m_storage;
    std::uint32_t m_count{0};
};

} // namespace adx::graph
