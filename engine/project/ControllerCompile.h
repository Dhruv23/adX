// adx-thread: main
//
// LFO, envelope, formula and keyboard controllers, compiled into straight-line knot
// chains (Controller.h).
//
// All four are pure functions of the arrangement - an LFO or a formula of position, an
// envelope or a keyboard controller of its source channel's notes - so they become
// automation lanes like any drawn curve and inherit everything lanes already guarantee:
// per-frame values, the same output offline and realtime, at any block size
// (phase_4.md §4.0). The peak controller follows a signal and cannot be compiled; it is
// graph::PeakControllerNode.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/TempoMap.h"
#include "engine/project/Controller.h"
#include "engine/project/EventTrack.h"
#include "engine/project/Formula.h"
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

class Project;
struct MacroTarget;

/// One parameter a controller's 0..1 output finally lands on, and how it gets there:
/// straight onto its target's range, or through one of a macro's targets (its min, max
/// and curve, clamped to the parameter's range).
struct ControllerDestination {
    ParamRef target;
    ControllerRange range;
    const MacroTarget* through{nullptr};

    [[nodiscard]] float map(float normalised) const noexcept;
};

/// Where a controller aimed at `target` writes: the parameter itself; each target of a
/// macro; or, for an X-Y pad's axis, wherever that axis drives. Empty for a target that
/// names nothing. Pointers are into `project` and live as long as it does unedited.
[[nodiscard]] std::vector<ControllerDestination> controllerDestinations(const Project& project,
                                                                        ParamRef target);

/// An X-Y pad's position on `axis` (kAxisX or kAxisY), and the target it drives.
[[nodiscard]] float padValue(const Controller& pad, std::uint16_t axis) noexcept;
[[nodiscard]] ParamRef padTarget(const Controller& pad, std::uint16_t axis) noexcept;

/// A formula is sampled every 1/64 note: fine enough that a straight line between two
/// samples is inaudible on any parameter a controller drives.
inline constexpr std::int64_t kFormulaStep = core::kPpq / 16;

/// The LFO's shape at `turns` (any real; one cycle per unit): -1..1. `seed` picks the
/// random shapes' levels.
[[nodiscard]] float lfoShapeAt(LfoShape shape, double turns, std::uint32_t seed = 1) noexcept;

/// Appends an LFO controller's chain over [0, endTick].
void compileLfo(std::vector<Knot>& out, const Controller& controller, ControllerRange range,
                std::int64_t endTick);

/// Appends a formula controller's chain over [0, endTick]: `formula` (the controller's,
/// compiled) sampled every kFormulaStep, or a single knot when it does not depend on
/// position. Nothing when it did not compile.
void compileFormula(std::vector<Knot>& out, const Controller& controller, const Formula& formula,
                    ControllerRange range, std::int64_t endTick, const core::TempoMap& tempo);

/// Appends a keyboard controller's chain: each note-on of its source channel moves the
/// output to that note's key (or velocity) over the controller's LOW..HIGH, gliding for
/// GLIDE ms, and it holds until the next. Empty when there are no notes.
void compileKeyboard(std::vector<Knot>& out, const Controller& controller, ControllerRange range,
                     std::span<const ScheduledEvent> events, const core::TempoMap& tempo);

/// Appends an envelope controller's chain: an ADSR retriggered at every note-on of its
/// source channel's `events` (sorted, as a snapshot's event track is), released at the
/// matching note-off. Times convert through `tempo`. Empty when there are no notes.
void compileEnvelope(std::vector<Knot>& out, const Controller& controller, ControllerRange range,
                     std::span<const ScheduledEvent> events, const core::TempoMap& tempo);

} // namespace adx::project
