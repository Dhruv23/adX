#include "engine/project/commands/MidiCommands.h"

#include <algorithm>

#include "engine/project/Project.h"

namespace adx::project {

namespace {

bool sameControl(const MidiBinding& a, const MidiBinding& b) noexcept {
    return a.device == b.device && a.channel == b.channel && a.kind == b.kind &&
           a.number == b.number;
}

} // namespace

AddMidiBinding::AddMidiBinding(MidiBinding binding) : m_binding(std::move(binding)) {}

void AddMidiBinding::apply(Project& project) {
    std::vector<MidiBinding>& bindings = project.midi.bindings;
    m_previous = bindings;
    std::erase_if(bindings, [this](const MidiBinding& b) { return sameControl(b, m_binding); });
    bindings.push_back(m_binding);
}

void AddMidiBinding::revert(Project& project) {
    project.midi.bindings = m_previous;
}

std::string_view AddMidiBinding::name() const noexcept {
    return "Learn MIDI control";
}

CommandId AddMidiBinding::kind() const noexcept {
    return CommandId::kAddMidiBinding;
}

DirtyMask AddMidiBinding::dirty() const noexcept {
    return dirty::kChannels;
}

RemoveMidiBinding::RemoveMidiBinding(std::size_t index) : m_index(index) {}

void RemoveMidiBinding::apply(Project& project) {
    std::vector<MidiBinding>& bindings = project.midi.bindings;
    m_removed.reset();
    if (m_index < bindings.size()) {
        m_removed = bindings[m_index];
        bindings.erase(bindings.begin() + static_cast<std::ptrdiff_t>(m_index));
    }
}

void RemoveMidiBinding::revert(Project& project) {
    if (m_removed) {
        std::vector<MidiBinding>& bindings = project.midi.bindings;
        const std::size_t at = std::min(m_index, bindings.size());
        bindings.insert(bindings.begin() + static_cast<std::ptrdiff_t>(at), *m_removed);
    }
}

std::string_view RemoveMidiBinding::name() const noexcept {
    return "Forget MIDI control";
}

CommandId RemoveMidiBinding::kind() const noexcept {
    return CommandId::kRemoveMidiBinding;
}

DirtyMask RemoveMidiBinding::dirty() const noexcept {
    return dirty::kChannels;
}

SetMidiRoutes::SetMidiRoutes(std::vector<MidiRouteSetting> routes) : m_routes(std::move(routes)) {}

void SetMidiRoutes::apply(Project& project) {
    m_previous = project.midi.routes;
    project.midi.routes = m_routes;
}

void SetMidiRoutes::revert(Project& project) {
    project.midi.routes = m_previous;
}

std::string_view SetMidiRoutes::name() const noexcept {
    return "Set MIDI routes";
}

CommandId SetMidiRoutes::kind() const noexcept {
    return CommandId::kSetMidiRoutes;
}

DirtyMask SetMidiRoutes::dirty() const noexcept {
    return dirty::kChannels;
}

} // namespace adx::project
