// The render snapshot: an immutable, flattened projection of a Project that the audio
// thread renders from and never looks past.
//
// FINAL_PLAN §4's commit protocol: a command mutates the main-thread project, the
// builder flattens it into one of these, the engine publishes the pointer through a
// lock-free queue, and the audio thread swaps it in *between blocks* and hands the
// old one to the reaper. The audio thread never dereferences the Project.
//
// Every member is a span or a value. There is no std::string, std::vector or
// shared_ptr here - this header is compiled into realtime code - and the storage the
// spans point into is owned elsewhere, by whatever `lifetime` destroys.
//
// Two members are written by the audio thread after publication, and both are
// documented as such: the event cursors, which are where each track's walk has got
// to, and the parameter values, which knob turns update in place without a rebuild.
// Neither is ever touched by the main thread once the snapshot is published.
#pragma once

#include <cstdint>
#include <span>

#include "engine/core/Curve.h"
#include "engine/core/TempoMath.h"
#include "engine/graph/RenderGraph.h"
#include "engine/project/EventTrack.h"
#include "engine/project/ParamRef.h"
#include "engine/rt/Reaper.h"

namespace adx::project {

/// Where a ParamRef's value lives in Snapshot::params. Sorted by ref, so the audio
/// thread finds a knob turn's target by binary search rather than by comparing paths.
struct ParamSlot {
    ParamRef ref;
    std::uint32_t index{0};
};

struct AutomationPoint {
    std::int64_t tick{0};
    float value{0.0F};
    core::Curve curve;
};

/// One automation clip as placed on the arrangement: absolute ticks, a target already
/// resolved to a parameter index, and its breakpoints.
struct AutomationLane {
    std::uint32_t paramIndex{graph::kNone};
    std::int64_t startTick{0};
    std::int64_t endTick{0};
    std::span<const AutomationPoint> points;

    /// The lane's value at `tick`, held flat outside its first and last points. The
    /// same rule AutomationClip::valueAt uses on the main thread.
    [[nodiscard]] float valueAt(std::int64_t tick) const noexcept;
};

struct Snapshot {
    /// The CommandStack revision this was built from.
    std::uint64_t revision{0};
    std::uint32_t sampleRate{48000};

    core::TempoView tempo;

    /// One track per channel that plays notes, in the order the render graph's steps
    /// name them.
    std::span<const EventTrack> eventTracks;
    /// Audio thread state: one cursor per track, for the source that drives it.
    std::span<EventCursor> cursors;

    std::span<const AutomationLane> automation;

    graph::RenderGraph graph;

    /// Audio thread state after publication: every resolved parameter value. A knob
    /// turn writes here through the message queue; structural edits rebuild.
    std::span<float> params;
    std::span<const ParamSlot> paramIndex;

    /// What the reaper destroys when this snapshot retires: the storage that owns
    /// every span above, on the main thread. The audio thread copies it into the
    /// reaper and never calls it.
    rt::Retired lifetime;

    /// The index of `ref` in `params`, or graph::kNone when the snapshot has no such
    /// parameter - the entity was deleted after the knob was grabbed.
    [[nodiscard]] std::uint32_t findParam(ParamRef ref) const noexcept;
};

/// The order ParamSlot is sorted in.
[[nodiscard]] bool paramRefLess(const ParamRef& lhs, const ParamRef& rhs) noexcept;

} // namespace adx::project
