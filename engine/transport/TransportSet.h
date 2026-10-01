// Every playback position the engine has, preallocated.
//
// Source 0 is the arrangement and is always in use. Phases 3-10 never touch the
// rest; they are inert array elements, and that is the point - the shape is right
// and the cost is one array (phase_3.md §4.1).
#pragma once

#include <array>
#include <bitset>
#include <cstdint>

#include "engine/transport/TimeSource.h"

namespace adx::transport {

class TransportSet {
public:
    TransportSet() noexcept;

    [[nodiscard]] TimeSource& arrangement() noexcept {
        return m_sources[0];
    }
    [[nodiscard]] const TimeSource& arrangement() const noexcept {
        return m_sources[0];
    }

    /// Any id, in use or not. An out-of-range id returns the arrangement rather than
    /// reading past the array: this is called on the audio thread, where a bad index
    /// must degrade, not fault.
    [[nodiscard]] TimeSource& get(TimeSourceId id) noexcept;
    [[nodiscard]] const TimeSource& get(TimeSourceId id) const noexcept;

    /// Claims a free source, reset to power-on state. kInvalidTimeSource when all
    /// kMaxTimeSources are taken. Never allocates. Phase 11.
    [[nodiscard]] TimeSourceId acquire() noexcept;
    /// Returns a source to the pool. Releasing the arrangement is refused.
    void release(TimeSourceId id) noexcept;

    [[nodiscard]] bool inUse(TimeSourceId id) const noexcept;
    [[nodiscard]] std::uint32_t inUseCount() const noexcept;

private:
    std::array<TimeSource, kMaxTimeSources> m_sources{};
    std::bitset<kMaxTimeSources> m_inUse;
};

} // namespace adx::transport
