// adx-thread: main
//
// LFO and envelope controllers, compiled into straight-line knot chains (Controller.h).
//
// Both are pure functions of the arrangement - an LFO of position, an envelope of where
// its source channel's notes start and end - so they become automation lanes like any
// drawn curve and inherit everything lanes already guarantee: per-frame values, the same
// output offline and realtime, at any block size (phase_4.md §4.0). The peak controller
// follows a signal and cannot be compiled; it is graph::PeakControllerNode.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/TempoMap.h"
#include "engine/project/Controller.h"
#include "engine/project/EventTrack.h"
#include "engine/project/Knot.h"
#include "engine/project/ParamDescriptor.h"

namespace adx::project {

/// A controller's output range: its target's descriptor, or 0..1 when it has none.
struct ControllerRange {
    float minimum{0.0F};
    float maximum{1.0F};
    bool logarithmic{false};

    [[nodiscard]] static ControllerRange of(const ParamDescriptor* descriptor) noexcept;

    /// `normalised` (clamped to 0..1) in the target's units.
    [[nodiscard]] float map(float normalised) const noexcept;
};

/// Segments per LFO cycle. 64 straight lines keep a sine within 0.12 % of its swing.
inline constexpr int kLfoSegmentsPerCycle = 64;

/// At most this many knots per controller; a long project with a fast LFO lowers the
/// resolution rather than the length.
inline constexpr std::size_t kMaxControllerKnots = 1U << 18U;

/// The LFO's shape at `turns` (any real; one cycle per unit): -1..1.
[[nodiscard]] float lfoShapeAt(LfoShape shape, double turns) noexcept;

/// Appends an LFO controller's chain over [0, endTick].
void compileLfo(std::vector<Knot>& out, const Controller& controller, ControllerRange range,
                std::int64_t endTick);

/// Appends an envelope controller's chain: an ADSR retriggered at every note-on of its
/// source channel's `events` (sorted, as a snapshot's event track is), released at the
/// matching note-off. Times convert through `tempo`. Empty when there are no notes.
void compileEnvelope(std::vector<Knot>& out, const Controller& controller, ControllerRange range,
                     std::span<const ScheduledEvent> events, const core::TempoMap& tempo);

} // namespace adx::project
