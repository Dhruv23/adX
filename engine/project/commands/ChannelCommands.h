// Commands on channels.
//
// Scalar fields share one command keyed by an enum rather than getting one class
// each. Nine near-identical classes would be nine places for the save-the-old-value
// discipline to be got wrong, and a fader drag and a knob drag want exactly the same
// coalescing behaviour anyway.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/core/Ids.h"
#include "engine/project/Channel.h"
#include "engine/project/Color.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

/// Channel fields that are a number, a flag or a small enum. All of them round-trip
/// through a double without loss: the widest is maxPolyphony at 16 bits.
enum class ChannelField : std::uint8_t {
    Volume,
    Pan,
    PitchCents,
    Muted,
    Soloed,
    MaxPolyphony,
    StealMode,
};

class AddChannel final : public Command {
public:
    /// The prototype's `id` is ignored; apply() assigns one.
    explicit AddChannel(Channel prototype);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::ChannelId created() const noexcept {
        return m_id;
    }

private:
    Channel m_prototype;
    core::ChannelId m_id;
    IdMarks m_marks;
};

/// Removes a channel and everything that referred to it.
///
/// "Everything" is the part that is easy to get wrong: a channel's note clips live
/// inside patterns, and leaving them behind would produce clips pointing at an id
/// that no longer exists. They are saved and restored with the channel, so undo puts
/// the notes back too.
class RemoveChannel final : public Command {
public:
    explicit RemoveChannel(core::ChannelId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    struct DetachedClip {
        core::PatternId pattern;
        std::size_t index{0};
        NoteClip clip;
    };
    struct DetachedMini {
        core::PatternId pattern;
        std::size_t index{0};
        MiniNotationSource source;
    };

    core::ChannelId m_id;
    std::size_t m_index{0};
    Channel m_previous;
    std::vector<DetachedClip> m_clips;
    std::vector<DetachedMini> m_minis;
    bool m_removed{false};
};

class SetChannelValue final : public Command {
public:
    SetChannelValue(core::ChannelId id, ChannelField field, double value);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::ChannelId m_id;
    ChannelField m_field{ChannelField::Volume};
    double m_value{0.0};
    double m_previous{0.0};
};

/// Renaming is its own command because a name is a *reference target*: automation
/// paths address channels by name, so the writer re-emits every path that pointed
/// here. Nothing else has to be updated, which is the point of resolving paths to
/// ids at load.
class RenameChannel final : public Command {
public:
    RenameChannel(core::ChannelId id, std::string channelName);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::ChannelId m_id;
    std::string m_name;
    std::string m_previous;
};

class SetChannelColor final : public Command {
public:
    SetChannelColor(core::ChannelId id, Color color);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::ChannelId m_id;
    Color m_color;
    Color m_previous;
};

class SetChannelOutput final : public Command {
public:
    SetChannelOutput(core::ChannelId id, core::InsertId output);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::ChannelId m_id;
    core::InsertId m_output;
    core::InsertId m_previous;
};

class SetInstrumentType final : public Command {
public:
    SetInstrumentType(core::ChannelId id, std::string type);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::ChannelId m_id;
    std::string m_type;
    std::string m_previous;
};

/// Sets a named instrument parameter, appending it if it is not there yet. The
/// append-in-order behaviour is what makes the writer's output stable: parameters
/// are emitted in declaration order, and declaration order is the order they were
/// first set.
class SetChannelParam final : public Command {
public:
    SetChannelParam(core::ChannelId id, std::string paramName, double value);
    SetChannelParam(core::ChannelId id, std::string paramName, double value, core::Curve curve);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::ChannelId m_id;
    ParamValue m_value;
    ParamValue m_previous;
    bool m_existed{false};
};

class SetChannelArp final : public Command {
public:
    SetChannelArp(core::ChannelId id, ArpSettings arp);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::ChannelId m_id;
    ArpSettings m_arp;
    ArpSettings m_previous;
};

} // namespace adx::project
