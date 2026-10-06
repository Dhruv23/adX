// adx-thread: main
//
// The view rectangle in musical/pitch space (phase_5.md §4.4).
//
// Two spaces meet here. **World space** is what geometry is built in and never
// changes with pan or zoom: x is beats (ticks / PPQ), y is `128 - pitch`, so row p
// spans y = 127 - p .. 128 - p and higher notes sit higher on screen. **Pixel space**
// is world space through the view transform, which the scene-graph item applies as a
// matrix - that is why a pan rebuilds nothing.
//
// A Viewport says which part of world space a build covers (it culls to it) and at
// what scale (it decides the level of detail). Python passes the visible rectangle
// grown by a margin, so panning inside the margin needs no rebuild at all.
#pragma once

#include <algorithm>
#include <cstdint>

#include "engine/core/Time.h"

namespace adx::geometry {

struct Viewport {
    /// Tick at the left edge (x = 0) and at the right edge.
    double tickStart{0.0};
    double tickEnd{static_cast<double>(core::kPpq) * 16.0};
    /// Pitch *value* at the bottom and top edges: row p covers values [p, p + 1), so
    /// pitchHigh = 72.0 puts the top edge exactly on top of row 71.
    float pitchLow{36.0F};
    float pitchHigh{84.0F};
    float pixelsPerTick{0.05F};
    float pixelsPerSemitone{12.0F};
    float widthPx{0.0F};
    float heightPx{0.0F};

    /// A viewport from its top-left corner, scale and size in pixels.
    [[nodiscard]] static Viewport make(double tickStart, float pitchTop, float pixelsPerTick,
                                       float pixelsPerSemitone, float widthPx,
                                       float heightPx) noexcept {
        const float ppt = std::max(pixelsPerTick, 1e-9F);
        const float pps = std::max(pixelsPerSemitone, 1e-6F);
        return Viewport{.tickStart = tickStart,
                        .tickEnd = tickStart + static_cast<double>(widthPx / ppt),
                        .pitchLow = pitchTop - (heightPx / pps),
                        .pitchHigh = pitchTop,
                        .pixelsPerTick = ppt,
                        .pixelsPerSemitone = pps,
                        .widthPx = widthPx,
                        .heightPx = heightPx};
    }

    [[nodiscard]] double tickAt(float x) const noexcept {
        return tickStart + static_cast<double>(x / pixelsPerTick);
    }
    /// The pitch value (not yet floored to a row) at pixel row y.
    [[nodiscard]] float pitchAt(float y) const noexcept {
        return pitchHigh - (y / pixelsPerSemitone);
    }
    [[nodiscard]] float xOf(double tick) const noexcept {
        return static_cast<float>(tick - tickStart) * pixelsPerTick;
    }
    [[nodiscard]] float yOf(float pitchValue) const noexcept {
        return (pitchHigh - pitchValue) * pixelsPerSemitone;
    }
};

/// World x of a tick.
[[nodiscard]] inline float worldX(std::int64_t ticks) noexcept {
    return static_cast<float>(static_cast<double>(ticks) / static_cast<double>(core::kPpq));
}

/// World y of the top of pitch row `pitch`.
[[nodiscard]] inline float worldRowTop(int pitch) noexcept {
    return static_cast<float>(127 - pitch);
}

} // namespace adx::geometry
