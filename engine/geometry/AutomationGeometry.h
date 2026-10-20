// adx-thread: main
//
// Breakpoint curves to vertices (phase_6.md §4.5): an automation lane, a clip envelope,
// or the miniature of an automation clip in the playlist.
//
// Every curved segment is tessellated with core::Curve::evaluate - the evaluator the
// engine compiles lanes with - so the line on screen and the value the audio thread
// plays cannot disagree (FINAL_PLAN §3.2). The UI never reimplements curve maths; it
// reads these vertices.
//
// World space: x is beats (ticks / PPQ), y is 1 - the value's position in its range,
// so 0 is the top of the lane and 1 the bottom. The item scales y by the lane's height.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/geometry/GeometryBuffer.h"

namespace adx::geometry {

/// One breakpoint as the UI hands it over: absolute (or item-relative) tick, value, the
/// shape of the segment that starts here.
struct LanePoint {
    std::int64_t tick{0};
    float value{0.0F};
    core::Curve curve;
};

/// A value range, and how to place a value in it.
struct LaneRange {
    float minimum{0.0F};
    float maximum{1.0F};
    bool logarithmic{false};

    /// `value`'s position in the range, 0..1 (clamped).
    [[nodiscard]] float normalise(float value) const noexcept;
    /// The inverse.
    [[nodiscard]] float denormalise(float position) const noexcept;
};

struct LaneView {
    double tickStart{0.0};
    double tickEnd{0.0};
    /// Pixels per tick and lane height in pixels: point squares and the tessellation
    /// step are sized in pixels, so a zoom rebuilds (cheaply).
    float pixelsPerTick{0.05F};
    float heightPx{80.0F};
    /// Lines per curved segment; 0 picks one per ~3 px.
    std::uint32_t segments{0};
    /// Where the lane sits in its item's world: added to every tick (a clip envelope's
    /// points are item-relative), and the band of world y it occupies. The defaults are a
    /// lane of its own: ticks as they are, y 0..1.
    double tickOffset{0.0};
    float yTop{0.0F};
    float yScale{1.0F};
};

/// Appends a line list tessellating `points` into `out` (three floats a vertex: x in
/// beats, y = 1 - normalised, role), offset by `tickOffset` and scaled into
/// [yTop, yTop + yScale]. Shared with PlaylistGeometry's automation clips.
void tessellateLane(std::vector<float>& out, std::span<const LanePoint> points,
                    const LaneRange& range, const LaneView& view, double tickOffset, float yTop,
                    float yScale, float role);

class AutomationGeometry {
public:
    /// `selected` holds one flag per point (or is empty).
    void build(std::span<const LanePoint> points, const LaneRange& range, const LaneView& view,
               std::span<const std::uint8_t> selected);

    [[nodiscard]] const GeometryBuffer& line() const noexcept {
        return m_line;
    }
    [[nodiscard]] const GeometryBuffer& points() const noexcept {
        return m_points;
    }
    [[nodiscard]] GeometryBuffer& buffer(std::size_t index) noexcept {
        return index == 0 ? m_line : m_points;
    }

private:
    GeometryBuffer m_line;
    GeometryBuffer m_points;
    std::vector<float> m_scratch;
};

/// The point (index) whose square is under pixel (x, y) of a lane drawn with `view`,
/// or -1. Squares are kPointPixels wide, as they are drawn.
[[nodiscard]] int hitTestLanePoint(std::span<const LanePoint> points, const LaneRange& range,
                                   const LaneView& view, float x, float y) noexcept;

/// The side of a point's square, in pixels.
inline constexpr float kPointPixels = 7.0F;

} // namespace adx::geometry
