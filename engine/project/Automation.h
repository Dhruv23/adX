// Automation lanes.
//
// A lane is a target plus a list of breakpoints, and the interpolation between two
// breakpoints is a core::Curve - the same evaluator the envelopes, the UI and the
// renderer use, so none of them can disagree about what "smooth" means
// (FINAL_PLAN §3.2).
#pragma once

#include <string>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/ParamRegistry.h"

namespace adx::project {

struct Breakpoint {
    core::Ticks at;
    float value{0.0F};
    /// The shape of the segment that *starts* here. The last breakpoint's curve is
    /// carried but unused, which is simpler than making the last one a different
    /// type and is what lets a user set the shape before drawing the next point.
    core::Curve curve;

    [[nodiscard]] friend bool operator==(const Breakpoint&, const Breakpoint&) noexcept = default;
};

struct AutomationClip {
    core::AutomationClipId id;

    /// The path exactly as the file spelled it, kept for two reasons: a lane whose
    /// target no longer resolves is still written back out unchanged rather than
    /// dropped, and a diagnostic can quote what the user actually typed.
    std::string targetPath;

    /// Resolved at load. `target.valid()` is false for a lane whose path did not
    /// resolve; those are reported as ADX3001 and preserved.
    ParamRef target;

    /// Sorted ascending by position. The parser sorts on load and reports ADX2007
    /// if it had to - v1 assumed the author had sorted them and misbehaved quietly
    /// when they had not.
    std::vector<Breakpoint> points;

    /// The lane's value at `at`, clamped to the first and last breakpoints outside
    /// its own range. Allocation-free and noexcept: Phase 3 calls it per lane per
    /// block.
    [[nodiscard]] float valueAt(core::Ticks at) const noexcept;

    [[nodiscard]] friend bool operator==(const AutomationClip&,
                                         const AutomationClip&) noexcept = default;
};

} // namespace adx::project
