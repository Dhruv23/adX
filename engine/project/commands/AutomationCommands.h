// Commands on automation lanes.
//
// Breakpoints are edited as a whole list rather than one at a time. A lane is small
// - a handful of points - and every gesture a user makes on one (add a point, drag
// one, delete a run, redraw a section) is expressible as "here is the new list". One
// command instead of four, with coalescing that works for a drag by construction.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/project/Automation.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

/// Adds an automation lane, either inside a pattern or directly in the playlist.
///
/// An invalid `pattern` means the playlist. That is not a sentinel for "nowhere":
/// the playlist owns lanes that are not part of any pattern, which is what a
/// timeline-level automation clip is.
class AddAutomationClip final : public Command {
public:
    AddAutomationClip(core::PatternId pattern, AutomationClip prototype);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::AutomationClipId created() const noexcept {
        return m_id;
    }

private:
    core::PatternId m_pattern;
    AutomationClip m_prototype;
    core::AutomationClipId m_id;
    IdMarks m_marks;
};

class RemoveAutomationClip final : public Command {
public:
    explicit RemoveAutomationClip(core::AutomationClipId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::AutomationClipId m_id;
    core::PatternId m_pattern;
    std::size_t m_index{0};
    AutomationClip m_previous;
    bool m_removed{false};
};

class SetBreakpoints final : public Command {
public:
    SetBreakpoints(core::AutomationClipId id, std::vector<Breakpoint> points);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::AutomationClipId m_id;
    std::vector<Breakpoint> m_points;
    std::vector<Breakpoint> m_previous;
};

} // namespace adx::project
