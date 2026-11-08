// adx-thread: main
#include "engine/project/commands/SessionCommands.h"

#include <utility>

#include "engine/project/Project.h"

namespace adx::project {

SetSession::SetSession(SessionModel session) : m_next(std::move(session)) {}

void SetSession::apply(Project& project) {
    m_previous = project.session;
    project.session = m_next;
}

void SetSession::revert(Project& project) {
    project.session = m_previous;
}

std::string_view SetSession::name() const noexcept {
    return "Edit session";
}

CommandId SetSession::kind() const noexcept {
    return CommandId::kSetSession;
}

DirtyMask SetSession::dirty() const noexcept {
    return dirty::kSession;
}

} // namespace adx::project
