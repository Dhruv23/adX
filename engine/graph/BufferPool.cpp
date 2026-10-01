#include "engine/graph/BufferPool.h"

#include "engine/core/Config.h"
#include "engine/graph/NodeId.h"

namespace adx::graph {

std::uint32_t assignBuffers(std::span<const LiveInterval> intervals,
                            std::span<std::uint32_t> assignment,
                            std::span<std::uint32_t> busyUntil) noexcept {
    ADX_ASSERT(assignment.size() >= intervals.size());
    ADX_ASSERT(busyUntil.size() >= intervals.size());

    std::uint32_t used = 0;
    for (std::size_t i = 0; i < intervals.size(); ++i) {
        const LiveInterval interval = intervals[i];
        std::uint32_t chosen = kNone;
        // First fit: the lowest-numbered buffer that is free by the time this interval
        // starts. Strictly before - the intervals are inclusive, and a step that reads a
        // buffer must not find it overwritten by a step that writes it in the same slot.
        for (std::uint32_t b = 0; b < used; ++b) {
            if (busyUntil[b] < interval.first) {
                chosen = b;
                break;
            }
        }
        if (chosen == kNone) {
            chosen = used;
            ++used;
        }
        busyUntil[chosen] = interval.last;
        assignment[i] = chosen;
    }
    return used;
}

std::span<float> BufferPool::channel(std::uint32_t buffer, std::uint32_t channelIndex,
                                     std::uint32_t offset, std::uint32_t frames) const noexcept {
    ADX_ASSERT(buffer < m_count);
    ADX_ASSERT(channelIndex < kPortChannels);
    ADX_ASSERT(offset + frames <= rt::kMaxBlockFrames);
    const std::size_t base = (static_cast<std::size_t>(buffer) * kFloatsPerBuffer) +
                             (static_cast<std::size_t>(channelIndex) * rt::kMaxBlockFrames) +
                             offset;
    return m_storage.subspan(base, frames);
}

} // namespace adx::graph
