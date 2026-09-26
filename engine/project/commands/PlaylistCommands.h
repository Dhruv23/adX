// Commands on the arrangement.
//
// Every one of these moves a *placement*. None of them touches a pattern's notes,
// which is the whole point of the Pattern/PlaylistItem split.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/Playlist.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

class AddPlaylistTrack final : public Command {
public:
    explicit AddPlaylistTrack(PlaylistTrack prototype);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::PlaylistTrackId created() const noexcept {
        return m_id;
    }

private:
    PlaylistTrack m_prototype;
    core::PlaylistTrackId m_id;
    IdMarks m_marks;
};

class RemovePlaylistTrack final : public Command {
public:
    explicit RemovePlaylistTrack(core::PlaylistTrackId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::PlaylistTrackId m_id;
    std::size_t m_index{0};
    PlaylistTrack m_previous;
    bool m_removed{false};
};

class RenamePlaylistTrack final : public Command {
public:
    RenamePlaylistTrack(core::PlaylistTrackId id, std::string trackName);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PlaylistTrackId m_id;
    std::string m_name;
    std::string m_previous;
};

class AddPlaylistItem final : public Command {
public:
    AddPlaylistItem(core::PlaylistTrackId track, PlaylistItem prototype);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::ItemId created() const noexcept {
        return m_id;
    }

private:
    core::PlaylistTrackId m_track;
    PlaylistItem m_prototype;
    core::ItemId m_id;
    IdMarks m_marks;
};

class RemovePlaylistItem final : public Command {
public:
    RemovePlaylistItem(core::PlaylistTrackId track, core::ItemId item);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::PlaylistTrackId m_track;
    core::ItemId m_item;
    std::size_t m_index{0};
    PlaylistItem m_previous;
    bool m_removed{false};
};

/// Moves an item along its track and optionally to another track. Coalesces, for
/// the same reason MoveNotes does.
class MovePlaylistItem final : public Command {
public:
    MovePlaylistItem(core::PlaylistTrackId track, core::ItemId item, core::Ticks start,
                     core::PlaylistTrackId destination);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PlaylistTrackId m_track;
    core::ItemId m_item;
    core::Ticks m_start;
    core::PlaylistTrackId m_destination;

    core::Ticks m_previousStart;
    std::size_t m_previousIndex{0};
    bool m_moved{false};
};

} // namespace adx::project
