// Commands for a project's MIDI setup (phase_9.md §3 step 2): learned bindings and
// input routes. Both rebuild the snapshot, which is where the audio thread reads them.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "engine/project/MidiSettings.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

/// Adds a binding, replacing any that listens to the same control (one control drives
/// one parameter: learning it again moves it).
class AddMidiBinding final : public Command {
public:
    explicit AddMidiBinding(MidiBinding binding);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    MidiBinding m_binding;
    std::vector<MidiBinding> m_previous;
};

class RemoveMidiBinding final : public Command {
public:
    explicit RemoveMidiBinding(std::size_t index);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    std::size_t m_index;
    std::optional<MidiBinding> m_removed;
};

class SetMidiRoutes final : public Command {
public:
    explicit SetMidiRoutes(std::vector<MidiRouteSetting> routes);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    std::vector<MidiRouteSetting> m_routes;
    std::vector<MidiRouteSetting> m_previous;
};

} // namespace adx::project
