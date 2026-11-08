// adx-thread: main
//
// Editing the session (phase_11.md §4.1). One command replaces the whole model: a session
// is a small grid, and every edit to it - a cell, a scene, a setting - is then one undo
// step with one shape, as SetMeta is for the project's properties.
#pragma once

#include "engine/project/Session.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

class SetSession final : public Command {
public:
    explicit SetSession(SessionModel session);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    SessionModel m_next;
    SessionModel m_previous;
};

} // namespace adx::project
