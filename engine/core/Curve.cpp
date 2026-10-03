#include "engine/core/Curve.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "engine/dsp/Math.h"

namespace adx::core {
namespace {

/// How each kind is spelled in `.adx`, indexed by CurveKind. Order must match the
/// enum; kNames.size() against kCurveKindCount is what keeps it that way.
constexpr std::array<std::string_view, kCurveKindCount> kNames{
    "linear", "exponential", "logarithmic", "step", "smooth", "bezier", "hold",
};
static_assert(kNames.size() == kCurveKindCount);

/// Exponent for Exponential and Logarithmic.
///
/// tension -1 gives 1 (linear), 0 gives 2 (quadratic), +1 gives 4. Exponential in
/// the tension so the control feels even across its range, which a linear map does
/// not.
[[nodiscard]] float exponentFor(float tension) noexcept {
    const float clamped = std::clamp(tension, -1.0F, 1.0F);
    return static_cast<float>(dsp::exp2(static_cast<double>(clamped) + 1.0));
}

/// x^e for x in [0, 1]. Through engine/dsp/Math.h rather than std::pow: this is the
/// evaluator automation and envelopes share, and its output reaches the golden
/// hashes, which every build configuration must reproduce (dsp/Math.h explains why a
/// library pow cannot promise that). The one include of engine/dsp from engine/core,
/// and a header-only one.
[[nodiscard]] float power(float x, float e) noexcept {
    if (x <= 0.0F) {
        return 0.0F;
    }
    return static_cast<float>(dsp::pow(static_cast<double>(x), static_cast<double>(e)));
}

/// x(s) for a cubic Bezier whose endpoints are 0 and 1.
[[nodiscard]] float bezierAxis(float s, float p1, float p2) noexcept {
    const float u = 1.0F - s;
    return (3.0F * u * u * s * p1) + (3.0F * u * s * s * p2) + (s * s * s);
}

/// dx/ds for the same curve, for Newton's method.
[[nodiscard]] float bezierAxisSlope(float s, float p1, float p2) noexcept {
    const float u = 1.0F - s;
    return (3.0F * u * u * p1) + (6.0F * u * s * (p2 - p1)) + (3.0F * s * s * (1.0F - p2));
}

/// Solves x(s) = target for s.
///
/// Newton first, because it converges in three or four steps for any handle
/// position somebody would actually draw. Bisection behind it for the rest: a
/// handle at x = 0 makes the slope vanish, and Newton alone would divide by it and
/// return NaN - which is exactly what curve_bezier_monotone exists to catch.
[[nodiscard]] float solveBezierParameter(float target, float p1, float p2) noexcept {
    constexpr int kNewtonSteps = 8;
    constexpr int kBisectSteps = 24;
    constexpr float kEpsilon = 1e-6F;

    float s = target;
    for (int i = 0; i < kNewtonSteps; ++i) {
        const float error = bezierAxis(s, p1, p2) - target;
        if (std::abs(error) < kEpsilon) {
            return std::clamp(s, 0.0F, 1.0F);
        }
        const float slope = bezierAxisSlope(s, p1, p2);
        if (std::abs(slope) < kEpsilon) {
            break;
        }
        s -= error / slope;
        if (!(s >= 0.0F) || !(s <= 1.0F)) {
            break;
        }
    }

    float low = 0.0F;
    float high = 1.0F;
    s = std::clamp(target, 0.0F, 1.0F);
    for (int i = 0; i < kBisectSteps; ++i) {
        const float x = bezierAxis(s, p1, p2);
        if (std::abs(x - target) < kEpsilon) {
            break;
        }
        if (x < target) {
            low = s;
        } else {
            high = s;
        }
        s = (low + high) * 0.5F;
    }
    return std::clamp(s, 0.0F, 1.0F);
}

} // namespace

const char* toString(CurveKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index < kNames.size() ? kNames.at(index).data() : "linear";
}

bool curveKindFromString(const char* name, std::size_t length, CurveKind& out) noexcept {
    if (name == nullptr) {
        return false;
    }
    const std::string_view candidate{name, length};
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (kNames.at(i) == candidate) {
            out = static_cast<CurveKind>(i);
            return true;
        }
    }
    return false;
}

float Curve::evaluate(float t) const noexcept {
    // Clamping rather than asserting: the renderer derives t from a block boundary,
    // and a block that straddles the last breakpoint would otherwise hand this
    // 1.0000001.
    const float x = std::clamp(t, 0.0F, 1.0F);

    switch (kind) {
    case CurveKind::Linear:
        return x;
    case CurveKind::Exponential:
        return power(x, exponentFor(tension));
    case CurveKind::Logarithmic:
        return 1.0F - power(1.0F - x, exponentFor(tension));
    case CurveKind::Step:
        return x < 1.0F ? 0.0F : 1.0F;
    case CurveKind::Smooth:
        return x * x * (3.0F - (2.0F * x));
    case CurveKind::Bezier: {
        // The handles' x coordinates have to stay inside [0,1] or x(s) is not
        // monotone and there is no single s to solve for.
        const float p1x = std::clamp(c1x, 0.0F, 1.0F);
        const float p2x = std::clamp(c2x, 0.0F, 1.0F);
        const float s = solveBezierParameter(x, p1x, p2x);
        const float u = 1.0F - s;
        return (3.0F * u * u * s * c1y) + (3.0F * u * s * s * c2y) + (s * s * s);
    }
    case CurveKind::Hold:
        return x > 0.0F ? 1.0F : 0.0F;
    }
    return x;
}

} // namespace adx::core
