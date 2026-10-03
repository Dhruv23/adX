// adx-thread: main
#include "engine/project/CurveCompile.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace adx::project {
namespace {

/// The largest gap between the curve and the chord of one piece, sampled at its
/// quarter points. Quarter points rather than only the midpoint because an S-curve's
/// chord crosses it at the midpoint.
[[nodiscard]] float pieceError(const core::Curve& curve, float v0, float v1, double a,
                               double b) noexcept {
    const float fa = curve.evaluate(static_cast<float>(a));
    const float fb = curve.evaluate(static_cast<float>(b));
    float worst = 0.0F;
    for (const double q : {0.25, 0.5, 0.75}) {
        const double t = a + ((b - a) * q);
        const float exact = curve.evaluate(static_cast<float>(t));
        const float chord = fa + ((fb - fa) * static_cast<float>(q));
        worst = std::max(worst, std::abs(exact - chord));
    }
    return worst * std::abs(v1 - v0);
}

} // namespace

void appendCurveSegment(std::vector<Knot>& out, std::int64_t t0, float v0, std::int64_t t1,
                        float v1, const core::Curve& curve, float tolerance) {
    if (t1 <= t0) {
        out.push_back(Knot{.tick = t1, .value = v1});
        return;
    }
    switch (curve.kind) {
    case core::CurveKind::Linear:
        out.push_back(Knot{.tick = t1, .value = v1});
        return;
    case core::CurveKind::Step:
        out.push_back(Knot{.tick = t1, .value = v0});
        out.push_back(Knot{.tick = t1, .value = v1});
        return;
    case core::CurveKind::Hold:
        out.push_back(Knot{.tick = t0, .value = v1});
        out.push_back(Knot{.tick = t1, .value = v1});
        return;
    default:
        break;
    }
    if (v0 == v1) {
        out.push_back(Knot{.tick = t1, .value = v1});
        return;
    }

    // Adaptive: split whichever piece is furthest from the curve, at its midpoint,
    // until every piece is within tolerance or there are kMaxCurvePieces of them. The
    // pieces gather where the curve bends - a Bezier's knee - instead of being spent
    // evenly on stretches that are already straight. A segment shorter than its piece
    // count in ticks cannot be split that finely, so ticks also bound the splitting.
    const std::int64_t length = t1 - t0;
    std::vector<double> cuts{0.0, 1.0};
    std::vector<float> errors{pieceError(curve, v0, v1, 0.0, 1.0)};
    while (static_cast<int>(errors.size()) < kMaxCurvePieces) {
        const auto worst = std::ranges::max_element(errors);
        if (*worst <= tolerance) {
            break;
        }
        const auto index = static_cast<std::size_t>(worst - errors.begin());
        const double a = cuts[index];
        const double b = cuts[index + 1];
        const double mid = 0.5 * (a + b);
        if ((b - a) * static_cast<double>(length) < 2.0) {
            *worst = 0.0F; // cannot split below a tick; accept it
            continue;
        }
        cuts.insert(cuts.begin() + static_cast<std::ptrdiff_t>(index) + 1, mid);
        errors[index] = pieceError(curve, v0, v1, a, mid);
        errors.insert(errors.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                      pieceError(curve, v0, v1, mid, b));
    }
    std::int64_t previous = t0;
    for (std::size_t k = 1; k < cuts.size(); ++k) {
        const bool last = k + 1 == cuts.size();
        const std::int64_t tick =
            last ? t1 : t0 + std::llround(cuts[k] * static_cast<double>(length));
        if (!last && tick <= previous) {
            continue;
        }
        // The value at the tick the cut rounded to, not at the cut itself, so the knot
        // lies exactly on the curve.
        const float value =
            last ? v1
                 : core::interpolate(curve, v0, v1,
                                     static_cast<float>(static_cast<double>(tick - t0) /
                                                        static_cast<double>(length)));
        out.push_back(Knot{.tick = tick, .value = value});
        previous = tick;
    }
}

float knotValueAt(const Knot* knots, std::size_t count, std::int64_t tick) noexcept {
    if (count == 0) {
        return 0.0F;
    }
    if (tick < knots[0].tick) {
        return knots[0].value;
    }
    // The last knot at or before `tick`: with two knots at one tick (a jump), the
    // later one - the value after the jump - is the one that applies at that tick.
    const Knot* end = knots + count;
    const Knot* after = std::upper_bound(knots, end, tick,
                                         [](std::int64_t t, const Knot& k) { return t < k.tick; });
    const Knot& from = *(after - 1);
    if (after == end || after->tick == from.tick) {
        return from.value;
    }
    const auto fraction =
        static_cast<double>(tick - from.tick) / static_cast<double>(after->tick - from.tick);
    return static_cast<float>(static_cast<double>(from.value) +
                              (static_cast<double>(after->value - from.value) * fraction));
}

} // namespace adx::project
