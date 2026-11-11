// adx-thread: main
//
// Macros folded into their targets when a snapshot is built (phase_12.md §4.2).
//
// A macro at value v puts each target at min + (max - min) * curve(v). That is written
// straight into the snapshot's parameter values, so a macro costs the audio thread
// nothing. Where a target also has drawn automation, the macro does not replace it: it
// offsets the lane in the target's normalised range, by the distance between where the
// macro puts the target and where the project's own value has it. A macro left where
// its targets already are changes nothing.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "engine/project/ControllerCompile.h"
#include "engine/project/Macro.h"

namespace adx::project {

class Project;

/// `value` as 0..1 of `range` (logarithmic where the range is), unclamped.
[[nodiscard]] float normalisedIn(const ControllerRange& range, float value) noexcept;

/// What a lane on a macro's target is moved by.
struct MacroOffset {
    ControllerRange range;
    float offset{0.0F};

    /// A lane value with the offset applied, clamped to the range.
    [[nodiscard]] float apply(float value) const noexcept;
};

/// By parameter index in the snapshot's storage.
using MacroOffsets = std::unordered_map<std::uint32_t, MacroOffset>;

/// Writes every resolvable macro target's value into `params` and returns the offset a
/// lane on each one takes. `indexOf` gives a parameter's index in `params`, or nothing
/// when no node reads it. Two macros on one target: the later one wins.
[[nodiscard]] MacroOffsets
foldMacros(const Project& project,
           const std::function<std::optional<std::uint32_t>(ParamRef)>& indexOf,
           std::vector<float>& params);

} // namespace adx::project
