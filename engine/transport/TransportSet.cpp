#include "engine/transport/TransportSet.h"

namespace adx::transport {

TransportSet::TransportSet() noexcept {
    m_inUse.set(0);
}

TimeSource& TransportSet::get(TimeSourceId id) noexcept {
    return id.v < kMaxTimeSources ? m_sources[id.v] : m_sources[0];
}

const TimeSource& TransportSet::get(TimeSourceId id) const noexcept {
    return id.v < kMaxTimeSources ? m_sources[id.v] : m_sources[0];
}

TimeSourceId TransportSet::acquire() noexcept {
    // Linear over 256 bits. Clip launches are human-rate events, and a bounded 256-step
    // scan is exactly the kind of work the audio thread is allowed to do.
    for (std::uint32_t i = 1; i < kMaxTimeSources; ++i) {
        if (!m_inUse.test(i)) {
            m_inUse.set(i);
            m_sources[i].reset();
            return TimeSourceId{i};
        }
    }
    return kInvalidTimeSource;
}

void TransportSet::release(TimeSourceId id) noexcept {
    if (id.v == 0 || id.v >= kMaxTimeSources) {
        return;
    }
    m_inUse.reset(id.v);
}

bool TransportSet::inUse(TimeSourceId id) const noexcept {
    return id.v < kMaxTimeSources && m_inUse.test(id.v);
}

std::uint32_t TransportSet::inUseCount() const noexcept {
    return static_cast<std::uint32_t>(m_inUse.count());
}

} // namespace adx::transport
