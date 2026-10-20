// adx-thread: main
#include "engine/geometry/AutomationGeometry.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Time.h"
#include "engine/dsp/Math.h"
#include "engine/geometry/ColorPalette.h"

namespace adx::geometry {
namespace {

constexpr double kPpq = static_cast<double>(core::kPpq);

void vertex(std::vector<float>& out, double tick, float y, float role) {
    out.push_back(static_cast<float>(tick / kPpq));
    out.push_back(y);
    out.push_back(role);
}

/// Lines for one segment, from point a to point b: straight for a linear curve,
/// subdivided otherwise. Step and Hold are drawn as the two straight lines they are.
void segment(std::vector<float>& out, const LanePoint& a, const LanePoint& b,
             const LaneRange& range, const LaneView& view, double tickOffset, float yTop,
             float yScale, float role) {
    const auto yOf = [&](float value) { return yTop + (yScale * (1.0F - range.normalise(value))); };
    const double t0 = static_cast<double>(a.tick) + tickOffset;
    const double t1 = static_cast<double>(b.tick) + tickOffset;
    const core::CurveKind kind = a.curve.kind;
    if (kind == core::CurveKind::Linear || t1 <= t0) {
        vertex(out, t0, yOf(a.value), role);
        vertex(out, t1, yOf(b.value), role);
        return;
    }
    if (kind == core::CurveKind::Step) {
        // Holds the start value, then jumps at the segment's end.
        vertex(out, t0, yOf(a.value), role);
        vertex(out, t1, yOf(a.value), role);
        vertex(out, t1, yOf(a.value), role);
        vertex(out, t1, yOf(b.value), role);
        return;
    }
    if (kind == core::CurveKind::Hold) {
        // Jumps at the segment's start, then holds the end value.
        vertex(out, t0, yOf(a.value), role);
        vertex(out, t0, yOf(b.value), role);
        vertex(out, t0, yOf(b.value), role);
        vertex(out, t1, yOf(b.value), role);
        return;
    }
    std::uint32_t lines = view.segments;
    if (lines == 0) {
        const double pixels = (t1 - t0) * static_cast<double>(view.pixelsPerTick);
        lines = static_cast<std::uint32_t>(std::clamp(pixels / 3.0, 4.0, 256.0));
    }
    double previousTick = t0;
    float previousY = yOf(a.value);
    for (std::uint32_t i = 1; i <= lines; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(lines);
        const double tick = t0 + ((t1 - t0) * static_cast<double>(t));
        const float y = yOf(core::interpolate(a.curve, a.value, b.value, t));
        vertex(out, previousTick, previousY, role);
        vertex(out, tick, y, role);
        previousTick = tick;
        previousY = y;
    }
}

} // namespace

float LaneRange::normalise(float value) const noexcept {
    if (!(maximum > minimum)) {
        return 0.0F;
    }
    if (logarithmic && minimum > 0.0F) {
        const double v = std::max(static_cast<double>(value), static_cast<double>(minimum));
        return static_cast<float>(std::clamp(
            dsp::log2(v / minimum) / dsp::log2(static_cast<double>(maximum) / minimum), 0.0, 1.0));
    }
    return std::clamp((value - minimum) / (maximum - minimum), 0.0F, 1.0F);
}

float LaneRange::denormalise(float position) const noexcept {
    const double p = std::clamp(static_cast<double>(position), 0.0, 1.0);
    if (logarithmic && minimum > 0.0F && maximum > minimum) {
        return static_cast<float>(minimum *
                                  dsp::exp2(p * dsp::log2(static_cast<double>(maximum) / minimum)));
    }
    return static_cast<float>(minimum + (p * (maximum - minimum)));
}

void tessellateLane(std::vector<float>& out, std::span<const LanePoint> points,
                    const LaneRange& range, const LaneView& view, double tickOffset, float yTop,
                    float yScale, float role) {
    if (points.empty()) {
        return;
    }
    const auto yOf = [&](float value) { return yTop + (yScale * (1.0F - range.normalise(value))); };
    // Held flat before the first point and after the last, as the engine plays it
    // (AutomationClip::valueAt).
    const double first = static_cast<double>(points.front().tick) + tickOffset;
    if (view.tickStart < first) {
        vertex(out, std::min(view.tickStart, first), yOf(points.front().value), role);
        vertex(out, first, yOf(points.front().value), role);
    }
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const double a = static_cast<double>(points[i].tick) + tickOffset;
        const double b = static_cast<double>(points[i + 1].tick) + tickOffset;
        if (b < view.tickStart || a > view.tickEnd) {
            continue; // culled
        }
        segment(out, points[i], points[i + 1], range, view, tickOffset, yTop, yScale, role);
    }
    const double last = static_cast<double>(points.back().tick) + tickOffset;
    if (view.tickEnd > last) {
        vertex(out, last, yOf(points.back().value), role);
        vertex(out, view.tickEnd, yOf(points.back().value), role);
    }
}

void AutomationGeometry::build(std::span<const LanePoint> points, const LaneRange& range,
                               const LaneView& view, std::span<const std::uint8_t> selected) {
    m_scratch.clear();
    tessellateLane(m_scratch, points, range, view, view.tickOffset, view.yTop, view.yScale,
                   roleValue(ColorRole::kAutomationLine));
    const std::span<float> line =
        m_line.resize(m_scratch.size() / kFloatsPerVertex, kFloatsPerVertex);
    std::ranges::copy(m_scratch, line.begin());

    // Squares a constant size in pixels: half-widths in world units at this zoom.
    const double halfX =
        (kPointPixels * 0.5) / std::max(static_cast<double>(view.pixelsPerTick), 1e-9) / kPpq;
    const float halfY = (kPointPixels * 0.5F) * view.yScale / std::max(view.heightPx, 1.0F);
    m_scratch.clear();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto tick = static_cast<double>(points[i].tick) + view.tickOffset;
        if (tick < view.tickStart - (halfX * kPpq) || tick > view.tickEnd + (halfX * kPpq)) {
            continue;
        }
        const bool chosen = i < selected.size() && selected[i] != 0;
        const float role =
            roleValue(chosen ? ColorRole::kAutomationPointSelected : ColorRole::kAutomationPoint);
        const auto x = static_cast<float>(tick / kPpq);
        const float y = view.yTop + (view.yScale * (1.0F - range.normalise(points[i].value)));
        const auto x0 = static_cast<float>(x - halfX);
        const auto x1 = static_cast<float>(x + halfX);
        const float y0 = y - halfY;
        const float y1 = y + halfY;
        for (const auto& [vx, vy] : {std::pair{x0, y0}, std::pair{x1, y0}, std::pair{x1, y1},
                                     std::pair{x0, y0}, std::pair{x1, y1}, std::pair{x0, y1}}) {
            m_scratch.insert(m_scratch.end(), {vx, vy, role});
        }
    }
    const std::span<float> squares =
        m_points.resize(m_scratch.size() / kFloatsPerVertex, kFloatsPerVertex);
    std::ranges::copy(m_scratch, squares.begin());
}

int hitTestLanePoint(std::span<const LanePoint> points, const LaneRange& range,
                     const LaneView& view, float x, float y) noexcept {
    const float half = (kPointPixels * 0.5F) + 1.0F;
    for (std::size_t i = points.size(); i-- > 0;) {
        const auto px = static_cast<float>((static_cast<double>(points[i].tick) - view.tickStart) *
                                           static_cast<double>(view.pixelsPerTick));
        const float py = (1.0F - range.normalise(points[i].value)) * view.heightPx;
        if (std::abs(px - x) <= half && std::abs(py - y) <= half) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace adx::geometry
