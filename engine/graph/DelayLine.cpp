#include "engine/graph/DelayLine.h"

#include <algorithm>

#include "engine/core/Config.h"

namespace adx::graph {

void DelayLine::attach(std::span<float> storage, std::uint32_t delay) noexcept {
    ADX_ASSERT(storage.size() >= storageFor(delay));
    m_storage = storage;
    m_delay = delay;
    m_writeIndex = 0;
}

void DelayLine::processAdd(std::span<const float> inLeft, std::span<const float> inRight,
                           std::span<float> outLeft, std::span<float> outRight) noexcept {
    const std::size_t frames = inLeft.size();
    if (m_delay == 0) {
        for (std::size_t i = 0; i < frames; ++i) {
            outLeft[i] += inLeft[i];
            outRight[i] += inRight[i];
        }
        return;
    }

    // Two rings side by side, one per channel, sharing a write position. Read the
    // oldest sample, write the newest in its place: a delay of exactly m_delay.
    const std::span<float> left = m_storage.first(m_delay);
    const std::span<float> right = m_storage.subspan(m_delay, m_delay);
    std::uint32_t index = m_writeIndex;
    for (std::size_t i = 0; i < frames; ++i) {
        outLeft[i] += left[index];
        outRight[i] += right[index];
        left[index] = inLeft[i];
        right[index] = inRight[i];
        ++index;
        if (index == m_delay) {
            index = 0;
        }
    }
    m_writeIndex = index;
}

void DelayLine::reset() noexcept {
    std::ranges::fill(m_storage, 0.0F);
    m_writeIndex = 0;
}

} // namespace adx::graph
