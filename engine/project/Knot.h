// One point of a straight-line chain: what every curve becomes before it reaches the
// audio thread (CurveCompile.h). Its own header because the render snapshot carries
// these, and realtime code may not include CurveCompile.h's <vector>.
#pragma once

#include <cstddef>
#include <cstdint>

namespace adx::project {

struct Knot {
    std::int64_t tick{0};
    float value{0.0F};

    [[nodiscard]] friend bool operator==(const Knot&, const Knot&) noexcept = default;
};

/// The chain's value at `tick`: held flat before the first knot and after the last,
/// linear between, and the later knot's value where two share a tick.
[[nodiscard]] float knotValueAt(const Knot* knots, std::size_t count, std::int64_t tick) noexcept;

} // namespace adx::project
