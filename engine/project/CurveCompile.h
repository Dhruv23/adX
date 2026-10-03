// adx-thread: main
//
// Curves to straight lines, once, on the main thread.
//
// The audio thread interpolates only straight lines: automation, clip envelopes and
// pitch slides all reach it as chains of linear knots. A curved segment - exponential,
// Bezier, smooth - is subdivided here until the chain is within a stated tolerance of
// core::Curve::evaluate, the one curve evaluator (phase_4.md §4.0). Two things follow:
// the per-sample cost of a curve is the cost of a line, and because the subdivision
// is a deterministic function of the curve, offline and realtime renders build the
// same chain and play the same samples.
#pragma once

#include <cstdint>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/project/Knot.h"

namespace adx::project {

/// The most pieces one curved segment becomes. A segment that still misses the
/// tolerance at this many is accepted as it is: 32 pieces of an 8-bar sweep are a
/// quarter-beat each, and nothing audible lives in the remainder.
inline constexpr int kMaxCurvePieces = 32;

/// Appends the knots that carry the value from (t0, v0) to (t1, v1) along `curve`.
/// The start knot is the caller's (it is the previous segment's end); every knot
/// after it, up to and including (t1, v1), is appended. Step and Hold become a flat
/// run plus a jump - two knots at one tick. `tolerance` is absolute, in the value's
/// own unit.
void appendCurveSegment(std::vector<Knot>& out, std::int64_t t0, float v0, std::int64_t t1,
                        float v1, const core::Curve& curve, float tolerance);

} // namespace adx::project
