// The one curve evaluator.
//
// Iteration one's patch editor had Bezier envelope handles that `.adx` could not
// represent, so save-and-reload silently flattened every curve (FINAL_PLAN
// §3.3.12). The fix is not "serialise the handles" - it is having exactly one
// curve type, which serialises, and which automation lanes, ADSR stages, the UI's
// curve drawing and the Phase 3 renderer all share so they cannot disagree
// (FINAL_PLAN §3.2).
//
// evaluate() is noexcept, allocation-free and total: it clamps its input, never
// returns NaN, and is called per automation point per block on the audio thread.
#pragma once

#include <cstddef>
#include <cstdint>

namespace adx::core {

/// The shape of the segment between two breakpoints.
///
/// Every kind is a normalised shape function on [0,1] -> [0,1]: it says how the
/// value travels between the two endpoints, not what the endpoints are. That is
/// what lets one evaluator serve automation, envelopes and the UI at once.
enum class CurveKind : std::uint8_t {
    /// f(t) = t.
    Linear,
    /// Ease-in, f(t) = t^k, with `tension` picking k.
    Exponential,
    /// Ease-out, the mirror of Exponential: f(t) = 1 - (1-t)^k.
    Logarithmic,
    /// Holds the segment's start value and jumps at its end: f(t) = t < 1 ? 0 : 1.
    /// This is iteration one's `step`, preserved exactly.
    Step,
    /// Smoothstep, f(t) = t^2 (3 - 2t). Eases in and out.
    Smooth,
    /// Cubic Bezier through (0,0) and (1,1) with the two handles below.
    Bezier,
    /// Jumps to the segment's end value immediately and holds it:
    /// f(t) = t > 0 ? 1 : 0. The sample-and-hold counterpart to Step.
    Hold,
};

/// Number of kinds, for table-driven tests and the writer's name table.
inline constexpr std::size_t kCurveKindCount = 7;

[[nodiscard]] const char* toString(CurveKind kind) noexcept;

/// Parses a curve name as `.adx` spells it. Returns false and leaves `out`
/// untouched when the name is not one of them - the caller turns that into a
/// diagnostic with a column, which is the half iteration one could not do.
[[nodiscard]] bool curveKindFromString(const char* name, std::size_t length,
                                       CurveKind& out) noexcept;

struct Curve {
    CurveKind kind{CurveKind::Linear};

    /// -1..1, used by Exponential and Logarithmic. 0 is the neutral shape
    /// (quadratic); -1 degenerates to linear and +1 is a hard knee.
    float tension{0.0F};

    /// Bezier control handles. The defaults are the flat-ish S a DAW draws when a
    /// segment is first switched to Bezier.
    float c1x{0.33F};
    float c1y{0.0F};
    float c2x{0.67F};
    float c2y{1.0F};

    /// t in [0,1] -> [0,1]. Total: clamps t, never throws, never returns NaN.
    [[nodiscard]] float evaluate(float t) const noexcept;

    [[nodiscard]] friend bool operator==(const Curve&, const Curve&) noexcept = default;
};

/// Applies the curve between two real values. One call site for automation, ADSR
/// and the UI, so "which end does tension bend toward" is answered once.
[[nodiscard]] inline float interpolate(const Curve& curve, float from, float to, float t) noexcept {
    return from + ((to - from) * curve.evaluate(t));
}

} // namespace adx::core
