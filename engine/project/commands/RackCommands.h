// adx-thread: main
//
// Macros, a channel's note filter, and inserting a rack (phase_12.md §4.1, §4.2).
//
// SetMacros replaces the project's whole macro list, as SetSession does the session:
// adding a macro, turning its knob and adding a target are all one command shape, and
// turning a knob coalesces into one history entry.
#pragma once

#include <vector>

#include "engine/project/Channel.h"
#include "engine/project/Macro.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

class SetMacros final : public Command {
public:
    /// Macros whose id is invalid get one when the command is applied.
    explicit SetMacros(std::vector<Macro> macros);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    std::vector<Macro> m_next;
    std::vector<Macro> m_previous;
    IdMarks m_marks;
};

/// The project's macros with macro `id` at `value` (clamped to 0..1): what a knob turn
/// hands SetMacros.
[[nodiscard]] std::vector<Macro> macrosWithValue(const Project& project, core::MacroId id,
                                                 float value);

class SetChannelNoteFilter final : public Command {
public:
    SetChannelNoteFilter(core::ChannelId id, NoteFilter filter);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::ChannelId m_id;
    NoteFilter m_filter;
    NoteFilter m_previous;
};

} // namespace adx::project
