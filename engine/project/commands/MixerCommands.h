// Commands on the mixer and the routing graph.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/project/Mixer.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

enum class InsertField : std::uint8_t {
    Gain,
    Pan,
    StereoSeparation,
    Muted,
    Soloed,
    PolarityInvert,
};

enum class SlotField : std::uint8_t { Mix, Bypass };

class AddInsert final : public Command {
public:
    explicit AddInsert(Insert prototype);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::InsertId created() const noexcept {
        return m_id;
    }

private:
    Insert m_prototype;
    core::InsertId m_id;
    IdMarks m_marks;
};

/// Removes an insert, every route touching it, every send aimed at it, and points
/// any channel that fed it at the master instead.
///
/// The alternative - leaving dangling references for Validate to complain about - is
/// how a delete turns into a corrupt project two saves later.
class RemoveInsert final : public Command {
public:
    explicit RemoveInsert(core::InsertId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    struct DetachedSend {
        core::InsertId owner;
        std::size_t index{0};
        Send send;
    };
    struct Rerouted {
        core::ChannelId channel;
        core::InsertId previous;
    };

    core::InsertId m_id;
    std::size_t m_index{0};
    Insert m_previous;
    std::vector<std::pair<std::size_t, Route>> m_routes;
    std::vector<DetachedSend> m_sends;
    std::vector<Rerouted> m_channels;
    /// The master, if this insert was it. Removing the master leaves the project
    /// without one, which Validate reports - rather than leaving `mixer.master`
    /// pointing at something that no longer exists, which nothing would notice.
    core::InsertId m_previousMaster;
    bool m_removed{false};
};

class RenameInsert final : public Command {
public:
    RenameInsert(core::InsertId id, std::string insertName);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::InsertId m_id;
    std::string m_name;
    std::string m_previous;
};

class SetInsertValue final : public Command {
public:
    SetInsertValue(core::InsertId id, InsertField field, double value);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::InsertId m_id;
    InsertField m_field{InsertField::Gain};
    double m_value{0.0};
    double m_previous{0.0};
};

class AddSlot final : public Command {
public:
    AddSlot(core::InsertId insert, Slot prototype);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::SlotId created() const noexcept {
        return m_id;
    }

private:
    core::InsertId m_insert;
    Slot m_prototype;
    core::SlotId m_id;
    IdMarks m_marks;
};

class RemoveSlot final : public Command {
public:
    explicit RemoveSlot(core::SlotId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::SlotId m_id;
    core::InsertId m_insert;
    std::size_t m_index{0};
    Slot m_previous;
    bool m_removed{false};
};

class SetSlotValue final : public Command {
public:
    SetSlotValue(core::SlotId id, SlotField field, double value);
    /// Sets a named effect parameter, appending it if it is not there yet.
    SetSlotValue(core::SlotId id, std::string paramName, double value);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::SlotId m_id;
    /// Empty means the field below; non-empty means a named parameter.
    std::string m_param;
    SlotField m_field{SlotField::Mix};
    double m_value{0.0};
    double m_previous{0.0};
    bool m_existed{false};
};

class AddSend final : public Command {
public:
    /// `wanted` adopts the id the file gave this send, so `insert.2.send.5.level`
    /// keeps meaning the same send across a save.
    AddSend(core::InsertId insert, core::InsertId target, float level, bool preFader,
            core::SendId wanted = {});

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::SendId created() const noexcept {
        return m_id;
    }

private:
    core::InsertId m_insert;
    core::InsertId m_target;
    float m_level{0.0F};
    bool m_preFader{false};
    core::SendId m_wanted;
    core::SendId m_id;
    IdMarks m_marks;
};

class RemoveSend final : public Command {
public:
    explicit RemoveSend(core::SendId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::SendId m_id;
    core::InsertId m_insert;
    std::size_t m_index{0};
    Send m_previous;
    bool m_removed{false};
};

class SetSendLevel final : public Command {
public:
    SetSendLevel(core::SendId id, float level);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::SendId m_id;
    float m_level{0.0F};
    float m_previous{0.0F};
};

class AddRoute final : public Command {
public:
    AddRoute(core::InsertId from, core::InsertId to, core::RouteId wanted = {});

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::RouteId created() const noexcept {
        return m_id;
    }

private:
    core::InsertId m_from;
    core::InsertId m_to;
    core::RouteId m_wanted;
    core::RouteId m_id;
    IdMarks m_marks;
};

class RemoveRoute final : public Command {
public:
    explicit RemoveRoute(core::RouteId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::RouteId m_id;
    std::size_t m_index{0};
    Route m_previous;
    bool m_removed{false};
};

class SetMaster final : public Command {
public:
    explicit SetMaster(core::InsertId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::InsertId m_id;
    core::InsertId m_previous;
};

} // namespace adx::project
