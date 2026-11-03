// adx-thread: main
//
// The oscilloscope (phase_10.md §4.4): the newest `windowFrames` of the master, each
// channel in its own half, aligned on a rising zero crossing so a steady tone stands
// still, and frozen while `hold` is set.
//
//   trigger   the latest rising crossing of the left channel that still leaves a full
//             window after it, searched over the frames before the newest window;
//             none found (silence, noise) and it free-runs on the newest frames
//   world     x in [0, 1] across the window; left in y 0..0.5, right in 0.5..1, each
//             with +1 at the top of its half
//   layer     0: both traces (lines)
#pragma once

#include <cstdint>
#include <span>

#include "engine/geometry/GeometryBuffer.h"
#include "engine/rt/OverwriteRing.h"

namespace adx::geometry {

struct ScopeStyle {
    std::uint32_t windowFrames{1024};
    bool trigger{true};
    bool hold{false};
    float gain{1.0F};
};

class ScopeGeometry {
public:
    /// `history`: oldest first; at least 2 * windowFrames for the trigger to find room.
    void build(std::span<const rt::StereoFrame> history, const ScopeStyle& style);

    [[nodiscard]] GeometryBuffer& lines() noexcept {
        return m_lines;
    }
    /// Index into the last history of the window's first frame: where it triggered.
    [[nodiscard]] std::size_t lastStart() const noexcept {
        return m_lastStart;
    }
    [[nodiscard]] bool triggered() const noexcept {
        return m_triggered;
    }

private:
    GeometryBuffer m_lines;
    std::size_t m_lastStart{0};
    bool m_triggered{false};
};

} // namespace adx::geometry
