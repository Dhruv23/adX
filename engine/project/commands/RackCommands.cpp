// adx-thread: main
#include "engine/project/commands/RackCommands.h"

#include <algorithm>
#include <utility>

namespace adx::project {

SetMacros::SetMacros(std::vector<Macro> macros) : m_next(std::move(macros)) {}

void SetMacros::apply(Project& project) {
    m_marks = project.idMarks();
    m_previous = project.macros;
    for (Macro& macro : m_next) {
        macro.id = project.newMacroId(macro.id);
    }
    project.macros = m_next;
}

void SetMacros::revert(Project& project) {
    project.macros = m_previous;
    project.restoreIdMarks(m_marks);
}

std::string_view SetMacros::name() const noexcept {
    return "Edit macros";
}

CommandId SetMacros::kind() const noexcept {
    return CommandId::kSetMacros;
}

DirtyMask SetMacros::dirty() const noexcept {
    return dirty::kChannels | dirty::kMixer;
}

bool SetMacros::coalesceWith(const Command& next) {
    // Only a knob turn folds in: the same macros and targets, other values.
    const auto& other = static_cast<const SetMacros&>(next);
    if (other.m_next.size() != m_next.size()) {
        return false;
    }
    for (std::size_t i = 0; i < m_next.size(); ++i) {
        Macro same = other.m_next[i];
        same.value = m_next[i].value;
        if (!(same == m_next[i])) {
            return false;
        }
    }
    m_next = other.m_next;
    return true;
}

std::vector<Macro> macrosWithValue(const Project& project, core::MacroId id, float value) {
    std::vector<Macro> macros = project.macros;
    for (Macro& macro : macros) {
        if (macro.id == id) {
            macro.value = std::clamp(value, 0.0F, 1.0F);
        }
    }
    return macros;
}

SetChannelNoteFilter::SetChannelNoteFilter(core::ChannelId id, NoteFilter filter)
    : m_id(id), m_filter(filter) {}

void SetChannelNoteFilter::apply(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel == nullptr) {
        return;
    }
    m_previous = channel->noteFilter;
    channel->noteFilter = m_filter;
}

void SetChannelNoteFilter::revert(Project& project) {
    if (Channel* channel = project.find(m_id)) {
        channel->noteFilter = m_previous;
    }
}

std::string_view SetChannelNoteFilter::name() const noexcept {
    return "Set key and velocity range";
}

CommandId SetChannelNoteFilter::kind() const noexcept {
    return CommandId::kSetChannelNoteFilter;
}

DirtyMask SetChannelNoteFilter::dirty() const noexcept {
    return dirty::kChannels | dirty::kPatterns;
}

} // namespace adx::project
