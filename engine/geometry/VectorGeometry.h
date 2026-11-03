// adx-thread: main
//
// The vectorscope and phase correlation (phase_10.md §4.4). The goniometer convention:
// mono (L = R) is a vertical line, a side-only signal (L = -R) a horizontal one, and a
// wide stereo image a cloud.
//
//   points      x = (R - L) / 2, y = -(L + R) / 2, in [-1, 1], up being positive mid
//   correlation sum(LR) / sqrt(sum(LL) sum(RR)) over the window: +1 mono, 0
//               uncorrelated, -1 out of phase (and 0 for silence), smoothed over frames
//   layers      0: the trace (lines between successive points),
//               1: the correlation meter under it (triangles), x from -1 to +1
#pragma once

#include <cstdint>
#include <span>

#include "engine/geometry/GeometryBuffer.h"
#include "engine/rt/OverwriteRing.h"

namespace adx::geometry {

struct VectorStyle {
    std::uint32_t windowFrames{2048};
    float gain{1.0F};
    /// Per-frame smoothing of the correlation readout, 0 (none) .. 1 (frozen).
    float smoothing{0.8F};
};

/// Correlation of `frames` alone, unsmoothed.
[[nodiscard]] float phaseCorrelation(std::span<const rt::StereoFrame> frames) noexcept;

class VectorGeometry {
public:
    void build(std::span<const rt::StereoFrame> history, const VectorStyle& style);

    [[nodiscard]] GeometryBuffer& trace() noexcept {
        return m_trace;
    }
    [[nodiscard]] GeometryBuffer& meter() noexcept {
        return m_meter;
    }
    [[nodiscard]] float correlation() const noexcept {
        return m_correlation;
    }

private:
    GeometryBuffer m_trace;
    GeometryBuffer m_meter;
    float m_correlation{0.0F};
};

} // namespace adx::geometry
