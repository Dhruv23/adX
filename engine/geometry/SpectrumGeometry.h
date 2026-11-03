// adx-thread: main
//
// The spectrum analyzer (phase_10.md §4.4): FFT magnitudes to a filled curve on a log
// frequency axis, with a peak-hold line that falls at a fixed rate.
//
//   columns   `columns` of them, 20 Hz to 20 kHz evenly in log frequency; each takes the
//             loudest bin it covers (so a narrow peak is never averaged away) or, where
//             a column is narrower than a bin, the bin under it
//   world     x in [0, 1] (20 Hz to 20 kHz), y 0 at 0 dB and 1 at `floorDb`
//   layers    0: the fill under the curve (triangles), 1: the curve and the peak line
//
// The magnitudes come from the AnalyzerHub's worker, which owns the FFT; this only
// lays them out, so it costs O(columns) per frame whatever the FFT size.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/geometry/GeometryBuffer.h"

namespace adx::geometry {

struct SpectrumStyle {
    std::uint32_t columns{384};
    float floorDb{-90.0F};
    bool peakHold{true};
    float peakFallDbPerSecond{12.0F};
};

class SpectrumGeometry {
public:
    /// `db`: one value per bin, bin k at k * sampleRate / (2 * (db.size() - 1)) Hz.
    void build(std::span<const float> db, std::uint32_t sampleRate, double dtSeconds,
               const SpectrumStyle& style);
    void resetPeaks() noexcept;

    [[nodiscard]] GeometryBuffer& fill() noexcept {
        return m_fill;
    }
    [[nodiscard]] GeometryBuffer& lines() noexcept {
        return m_lines;
    }
    /// The curve's column values in dB, for tests and readouts.
    [[nodiscard]] std::span<const float> columns() const noexcept {
        return m_columns;
    }
    [[nodiscard]] std::span<const float> peaks() const noexcept {
        return m_peaks;
    }

    /// x of `hz` in world space.
    [[nodiscard]] static float xOf(double hz) noexcept;

private:
    GeometryBuffer m_fill;
    GeometryBuffer m_lines;
    std::vector<float> m_columns;
    std::vector<float> m_peaks;
};

} // namespace adx::geometry
