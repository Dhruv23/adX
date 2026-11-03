// Commands for hosted plugins (phase_9.md §4.7).
//
// A plugin's state lives in the plugin until it is asked for it; this is the one way
// it reaches the project - on save, before an export, or after the user touched the
// plugin's own editor. It changes what the file says, not what plays, so no node is
// rebuilt for it.
#pragma once

#include <cstdint>
#include <string>

#include "engine/core/Ids.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

class SetPluginState final : public Command {
public:
    /// A slot's plugin.
    SetPluginState(core::SlotId slot, std::string state);
    /// A channel's plugin instrument.
    SetPluginState(core::ChannelId channel, std::string state);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    [[nodiscard]] std::string* target(Project& project) const;

    bool m_onSlot{true};
    std::uint32_t m_owner{0};
    std::string m_state;
    std::string m_previous;
    bool m_applied{false};
};

} // namespace adx::project
