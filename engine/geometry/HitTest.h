// adx-thread: main
//
// Point and rectangle to note ids, in C++ (phase_5.md §4.6).
//
// Rule 2 applied to input: a rubber band over 10,000 notes is one call that fills a
// caller-provided span and returns a count, not 10,000 Python comparisons. Coordinates
// are pixels in the viewport's frame (Viewport.h), which is what a mouse event carries.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/core/Ids.h"
#include "engine/geometry/PianoRollGeometry.h"
#include "engine/geometry/Viewport.h"
#include "engine/project/Project.h"

namespace adx::geometry {

/// Which part of a note the point is on. The draw tool resizes from an edge and moves
/// from the body.
enum class NotePart : std::uint8_t { kNone, kBody, kLeftEdge, kRightEdge };

struct NoteHit {
    core::NoteId note;
    NotePart part{NotePart::kNone};
};

/// An edge is this many pixels deep, or a third of the note if it is narrower.
inline constexpr float kEdgePixels = 6.0F;

struct Rect {
    float x0{0.0F};
    float y0{0.0F};
    float x1{0.0F};
    float y1{0.0F};
};

struct LaneHit {
    core::NoteId note;
    /// The lane value at the point, 0..1 (top is 1).
    float value{0.0F};
};

/// The topmost note under (x, y): of overlapping notes, the one stored last, which is
/// the one drawn last.
[[nodiscard]] NoteHit hitTestNote(const project::Project& project, core::PatternId pattern,
                                  core::ChannelId channel, const Viewport& viewport, float x,
                                  float y);

/// Every note the rectangle touches. Writes up to out.size() ids and returns how many
/// there are in total, so a caller whose span was too small can tell.
[[nodiscard]] std::size_t hitTestRect(const project::Project& project, core::PatternId pattern,
                                      core::ChannelId channel, const Viewport& viewport, Rect rect,
                                      std::span<core::NoteId> out);

/// The note whose lane bar is under x, in a lane `laneHeightPx` tall with y measured
/// from its top. Bars are kLaneBarPixels wide (or the note, if shorter); the nearest
/// bar start within that wins.
[[nodiscard]] LaneHit hitTestLane(const project::Project& project, core::PatternId pattern,
                                  core::ChannelId channel, const Viewport& viewport,
                                  float laneHeightPx, float x, float y);

} // namespace adx::geometry
