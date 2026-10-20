// adx-thread: main
//
// Every mixer strip's meter in one vertex buffer (phase_6.md §4.3).
//
// The mixer reads one row of levels per insert in the frame clock's single engine call,
// and this turns the whole array into one buffer - so 200 inserts cost one FFI call and
// one upload a frame, not 200 (FINAL_PLAN §2.2: "one FFI call per frame, total"). The
// build happens inside that same call (Engine.frame), which is why it is O(strips) and
// allocation-free once warm.
//
// World space: strip i spans x in [i, i + 1), two bars (left, right) inside it; y is 0
// at 0 dBFS and 1 at the floor. Clip indicators latch until reset(), like a desk's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/geometry/GeometryBuffer.h"
#include "engine/rt/OverwriteRing.h"

namespace adx::geometry {

/// The meter's floor, and where the bar turns hot.
inline constexpr float kMeterFloorDb = -60.0F;
inline constexpr float kMeterHotDb = -6.0F;

/// A level (linear amplitude) as a position from the top: 0 at 0 dBFS, 1 at the floor.
[[nodiscard]] float meterPosition(float amplitude) noexcept;

class MeterGeometry {
public:
    /// One strip per frame, in the order the mixer lays the strips out.
    void build(std::span<const rt::LevelFrame> strips);

    /// Clears every latched clip indicator.
    void resetClips() noexcept;

    [[nodiscard]] const GeometryBuffer& bars() const noexcept {
        return m_bars;
    }
    [[nodiscard]] GeometryBuffer& buffer() noexcept {
        return m_bars;
    }
    /// Whether strip `index` has clipped since the last reset.
    [[nodiscard]] bool clipped(std::size_t index) const noexcept {
        return index < m_clipped.size() && m_clipped[index] != 0;
    }

private:
    GeometryBuffer m_bars;
    std::vector<std::uint8_t> m_clipped;
};

} // namespace adx::geometry
