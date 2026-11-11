// A macro: one 0..1 knob over several parameters (phase_12.md §4.2).
//
// A rack exposes "Tail Length" or "Crunch" as one control; each target says which
// parameter it moves, between which two values, and along which curve. A macro is a
// parameter itself (`macro.<name>`), so a lane can be drawn on it. Macros are folded
// into their targets when a snapshot is built (MacroCompile.h): the audio thread never
// sees one.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/core/Ids.h"
#include "engine/project/ParamRef.h"

namespace adx::project {

struct MacroTarget {
    /// As written and as resolved, like a controller's target: a path that no longer
    /// resolves is kept and written back.
    std::string path;
    ParamRef target;
    /// The target's value at macro 0 and at macro 1, in the target's own units.
    float minimum{0.0F};
    float maximum{1.0F};
    core::Curve curve;

    [[nodiscard]] friend bool operator==(const MacroTarget&, const MacroTarget&) noexcept = default;
};

struct Macro {
    core::MacroId id;
    /// Unique within the project: automation addresses a macro by it.
    std::string name;
    float value{0.5F};
    std::vector<MacroTarget> targets;

    [[nodiscard]] friend bool operator==(const Macro&, const Macro&) noexcept = default;
};

/// A rack exposes at most this many (phase_12.md §4.2). The project itself may hold
/// more: several racks each bring their own.
inline constexpr std::size_t kRackMacroSlots = 8;

/// The target's value with the macro at `value`: min + (max - min) * curve(value).
[[nodiscard]] float macroTargetValue(const MacroTarget& target, float value) noexcept;

} // namespace adx::project
