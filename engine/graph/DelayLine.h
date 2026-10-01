// An integer-sample delay, used by plugin delay compensation.
//
// Integer only. Fractional compensation is not a thing: a sub-sample offset from a
// resampler is that resampler's problem to declare as an integer (phase_3.md §4.6).
// The storage is the builder's, sized to exactly the delay this line applies, so a
// line never grows and never allocates.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/graph/PortSpec.h"

namespace adx::graph {

class DelayLine {
public:
    DelayLine() noexcept = default;

    /// Floats of storage a line of `delay` samples needs.
    [[nodiscard]] static constexpr std::size_t storageFor(std::uint32_t delay) noexcept {
        return static_cast<std::size_t>(kPortChannels) * delay;
    }

    /// Main thread. `storage` holds storageFor(delay) floats, starts zeroed, and
    /// outlives the line.
    void attach(std::span<float> storage, std::uint32_t delay) noexcept;

    /// Adds the input, delayed, into `out` - which is how an edge feeds an accumulator.
    /// `in` and `out` are planar stereo, `frames` long.
    void processAdd(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight) noexcept;

    [[nodiscard]] std::uint32_t delay() const noexcept {
        return m_delay;
    }

    /// Clears the history.
    void reset() noexcept;

private:
    std::span<float> m_storage;
    std::uint32_t m_delay{0};
    std::uint32_t m_writeIndex{0};
};

} // namespace adx::graph
