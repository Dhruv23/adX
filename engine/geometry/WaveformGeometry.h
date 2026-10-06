// adx-thread: main
//
// Mipmapped peaks to vertices (phase_5.md §4.5).
//
// A triangle strip of min/max pairs, two vertices per pixel column, so the cost is
// O(pixels) and never O(samples): a 10-minute file 1000 columns wide is 2,000
// vertices at every zoom. World space is x in seconds from the start of the source and
// y in amplitude (-1..1, up positive); the view transform does the rest, so a pan
// inside the built range rebuilds nothing.
#pragma once

#include <cstdint>

#include "engine/geometry/ClipPeakCache.h"
#include "engine/geometry/GeometryBuffer.h"

namespace adx::geometry {

struct WaveView {
    double frameStart{0.0};
    double frameEnd{0.0};
    std::uint32_t columns{0};
};

class WaveformGeometry {
public:
    /// Reads whatever tiers are ready and never waits for the rest. With nothing ready
    /// it draws a flat line in ColorRole::kWavePending, so the clip is visible at once.
    void build(const ClipPeaks& peaks, const WaveView& view);

    [[nodiscard]] const GeometryBuffer& strip() const noexcept {
        return m_strip;
    }
    [[nodiscard]] GeometryBuffer& strip() noexcept {
        return m_strip;
    }
    /// The tier the last build read, or -1 when it drew the pending line.
    [[nodiscard]] int tier() const noexcept {
        return m_tier;
    }

private:
    GeometryBuffer m_strip;
    int m_tier{-1};
};

} // namespace adx::geometry
