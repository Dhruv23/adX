// adx-thread: main
//
// The few operations the Python scripting API needs that are not one existing command
// (phase_7.md §4.7). Everything else a script does is a command builder the GUI already
// uses (bindings/edit.cpp) - which is the point: FINAL_PLAN §4 requires the API to have
// no privileged mutation path, and a curated surface that executed commands of its
// own kind would be one. So every function here *is* commands, executed on the stack:
//
//   - positions and durations in the file's own spelling, so a script says
//     `at="4:0:0"` exactly as the file would;
//   - mini-notation compiled into ordinary notes, or set as a live MINI block;
//   - an effect added with its parameters as one undo step, returning its id.
//
// Main thread only, like every command.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/Note.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::script {

/// `bar:beat:tick`, `bar:beat` or decimal beats, read against the project's tempo map
/// exactly as the parser reads a position. Throws std::invalid_argument naming what is
/// wrong.
[[nodiscard]] core::Ticks positionOf(const project::Project& project, std::string_view text);

/// A duration in the same spelling: `2:0:0` is two bars.
[[nodiscard]] core::Ticks durationOf(const project::Project& project, std::string_view text);

/// The notes `text` plays over a pattern `length` long, its cycles counted from the
/// pattern's start (a script writing notes into a pattern means the pattern's bars,
/// not the song's). Throws std::invalid_argument with the compiler's message when the
/// text does not parse.
[[nodiscard]] std::vector<project::Note> miniNotes(std::string_view text, core::Ticks length,
                                                   core::Ticks cycle);

/// Adds miniNotes() for the pattern to (pattern, channel) as one command. Returns how
/// many notes it added.
std::size_t addMiniNotes(project::Project& project, project::CommandStack& stack,
                         core::PatternId pattern, core::ChannelId channel, std::string_view text);

/// Sets (or, given empty text, removes) a live MINI block: one command. The text is
/// split on newlines into the block's lines.
void setMini(project::Project& project, project::CommandStack& stack, core::PatternId pattern,
             core::ChannelId channel, std::string_view text);

/// Adds an effect to an insert's chain with its parameters set: one command. Returns
/// the new slot's id. Throws std::invalid_argument for an insert that does not exist.
core::SlotId addEffect(project::Project& project, project::CommandStack& stack,
                       core::InsertId insert, std::string type,
                       const std::vector<std::pair<std::string, double>>& params, float mix);

} // namespace adx::script
