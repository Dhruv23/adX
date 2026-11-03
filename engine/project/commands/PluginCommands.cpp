#include "engine/project/commands/PluginCommands.h"

#include "engine/project/Project.h"

namespace adx::project {

SetPluginState::SetPluginState(core::SlotId slot, std::string state)
    : m_owner(slot.value), m_state(std::move(state)) {}

SetPluginState::SetPluginState(core::ChannelId channel, std::string state)
    : m_onSlot(false), m_owner(channel.value), m_state(std::move(state)) {}

std::string* SetPluginState::target(Project& project) const {
    if (m_onSlot) {
        Slot* slot = project.mixer.findSlot(core::SlotId{m_owner});
        return slot != nullptr ? &slot->plugin.state : nullptr;
    }
    Channel* channel = project.find(core::ChannelId{m_owner});
    return channel != nullptr ? &channel->instrument.plugin.state : nullptr;
}

void SetPluginState::apply(Project& project) {
    std::string* state = target(project);
    m_applied = state != nullptr;
    if (state != nullptr) {
        m_previous = *state;
        *state = m_state;
    }
}

void SetPluginState::revert(Project& project) {
    if (std::string* state = target(project); state != nullptr && m_applied) {
        *state = m_previous;
    }
    m_applied = false;
}

std::string_view SetPluginState::name() const noexcept {
    return "Store plugin state";
}

CommandId SetPluginState::kind() const noexcept {
    return CommandId::kSetPluginState;
}

DirtyMask SetPluginState::dirty() const noexcept {
    // The file changes; the graph does not (the live plugin already has this state).
    return m_onSlot ? dirty::kMixer : dirty::kChannels;
}

std::uint64_t SetPluginState::targetKey() const noexcept {
    return (static_cast<std::uint64_t>(m_onSlot ? 1 : 2) << 32U) | m_owner;
}

bool SetPluginState::coalesceWith(const Command& next) {
    // Successive syncs of one plugin are one history entry: the oldest previous state,
    // the newest state.
    m_state = static_cast<const SetPluginState&>(next).m_state;
    return true;
}

} // namespace adx::project
