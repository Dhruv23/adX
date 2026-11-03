// adx-thread: main
//
// The loudness meter (phase_10.md §4.4): the master's BS.1770 readings, which the audio
// thread already measures (Phase 4 §4.10, LevelFrame), as three bars - momentary,
// short-term, integrated - and a scrolling history of the short-term reading.
//
//   world     bars at x 0..0.3 (three columns), history at x 0.35..1, newest at the
//             right; y 0 at 0 LUFS and 1 at `floorLufs`
//   layers    0: bars (triangles), 1: history (lines)
//   history   a fixed ring of `historyFrames` readings, one per build
#pragma once

#include <cstdint>
#include <vector>

#include "engine/geometry/GeometryBuffer.h"
#include "engine/rt/OverwriteRing.h"

namespace adx::geometry {

struct LoudnessStyle {
    float floorLufs{-60.0F};
    std::uint32_t historyFrames{600};
};

class LoudnessGeometry {
public:
    void build(const rt::LevelFrame& master, const LoudnessStyle& style);
    void reset() noexcept;

    [[nodiscard]] GeometryBuffer& bars() noexcept {
        return m_bars;
    }
    [[nodiscard]] GeometryBuffer& history() noexcept {
        return m_history;
    }
    [[nodiscard]] float momentary() const noexcept {
        return m_last.momentary;
    }
    [[nodiscard]] float shortTerm() const noexcept {
        return m_last.shortTerm;
    }
    [[nodiscard]] float integrated() const noexcept {
        return m_last.integrated;
    }

private:
    GeometryBuffer m_bars;
    GeometryBuffer m_history;
    std::vector<float> m_ring;
    std::size_t m_head{0};
    std::size_t m_filled{0};
    rt::LevelFrame m_last;
};

} // namespace adx::geometry
