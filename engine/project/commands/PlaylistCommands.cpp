#include "engine/project/commands/PlaylistCommands.h"

#include <algorithm>
#include <utility>

namespace adx::project {

// --- AddPlaylistTrack --------------------------------------------------------

AddPlaylistTrack::AddPlaylistTrack(PlaylistTrack prototype) : m_prototype(std::move(prototype)) {}

void AddPlaylistTrack::apply(Project& project) {
    m_marks = project.idMarks();
    m_id = project.newPlaylistTrackId(m_prototype.id);
    PlaylistTrack track = m_prototype;
    track.id = m_id;
    for (PlaylistItem& item : track.items) {
        item.id = project.newItemId(item.id);
    }
    project.playlist.tracks.push_back(std::move(track));
}

void AddPlaylistTrack::revert(Project& project) {
    std::erase_if(project.playlist.tracks,
                  [this](const PlaylistTrack& track) { return track.id == m_id; });
    project.restoreIdMarks(m_marks);
    m_id = core::PlaylistTrackId{};
}

std::string_view AddPlaylistTrack::name() const noexcept {
    return "Add playlist track";
}

CommandId AddPlaylistTrack::kind() const noexcept {
    return CommandId::kAddPlaylistTrack;
}

DirtyMask AddPlaylistTrack::dirty() const noexcept {
    return dirty::kPlaylist;
}

// --- RemovePlaylistTrack -----------------------------------------------------

RemovePlaylistTrack::RemovePlaylistTrack(core::PlaylistTrackId id) : m_id(id) {}

void RemovePlaylistTrack::apply(Project& project) {
    auto& tracks = project.playlist.tracks;
    const auto match = std::ranges::find(tracks, m_id, &PlaylistTrack::id);
    m_removed = match != tracks.end();
    if (!m_removed) {
        return;
    }
    m_index = static_cast<std::size_t>(std::distance(tracks.begin(), match));
    m_previous = *match;
    tracks.erase(match);
}

void RemovePlaylistTrack::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    auto& tracks = project.playlist.tracks;
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, tracks.size()));
    tracks.insert(tracks.begin() + at, m_previous);
}

std::string_view RemovePlaylistTrack::name() const noexcept {
    return "Remove playlist track";
}

CommandId RemovePlaylistTrack::kind() const noexcept {
    return CommandId::kRemovePlaylistTrack;
}

DirtyMask RemovePlaylistTrack::dirty() const noexcept {
    return dirty::kPlaylist;
}

// --- RenamePlaylistTrack -----------------------------------------------------

RenamePlaylistTrack::RenamePlaylistTrack(core::PlaylistTrackId id, std::string trackName)
    : m_id(id), m_name(std::move(trackName)) {}

void RenamePlaylistTrack::apply(Project& project) {
    PlaylistTrack* track = project.playlist.find(m_id);
    if (track == nullptr) {
        return;
    }
    m_previous = track->name;
    track->name = m_name;
}

void RenamePlaylistTrack::revert(Project& project) {
    PlaylistTrack* track = project.playlist.find(m_id);
    if (track != nullptr) {
        track->name = m_previous;
    }
}

std::string_view RenamePlaylistTrack::name() const noexcept {
    return "Rename playlist track";
}

CommandId RenamePlaylistTrack::kind() const noexcept {
    return CommandId::kRenamePlaylistTrack;
}

DirtyMask RenamePlaylistTrack::dirty() const noexcept {
    return dirty::kPlaylist;
}

std::uint64_t RenamePlaylistTrack::targetKey() const noexcept {
    return m_id.value;
}

bool RenamePlaylistTrack::coalesceWith(const Command& next) {
    m_name = static_cast<const RenamePlaylistTrack&>(next).m_name;
    return true;
}

// --- AddPlaylistItem ---------------------------------------------------------

AddPlaylistItem::AddPlaylistItem(core::PlaylistTrackId track, PlaylistItem prototype)
    : m_track(track), m_prototype(prototype) {}

void AddPlaylistItem::apply(Project& project) {
    PlaylistTrack* track = project.playlist.find(m_track);
    if (track == nullptr) {
        return;
    }
    m_marks = project.idMarks();
    m_id = project.newItemId(m_prototype.id);
    PlaylistItem item = m_prototype;
    item.id = m_id;
    track->items.push_back(item);
}

void AddPlaylistItem::revert(Project& project) {
    if (!m_id.valid()) {
        return;
    }
    PlaylistTrack* track = project.playlist.find(m_track);
    if (track != nullptr) {
        std::erase_if(track->items, [this](const PlaylistItem& item) { return item.id == m_id; });
    }
    project.restoreIdMarks(m_marks);
    m_id = core::ItemId{};
}

std::string_view AddPlaylistItem::name() const noexcept {
    return "Place clip";
}

CommandId AddPlaylistItem::kind() const noexcept {
    return CommandId::kAddPlaylistItem;
}

DirtyMask AddPlaylistItem::dirty() const noexcept {
    return dirty::kPlaylist;
}

// --- RemovePlaylistItem ------------------------------------------------------

RemovePlaylistItem::RemovePlaylistItem(core::PlaylistTrackId track, core::ItemId item)
    : m_track(track), m_item(item) {}

void RemovePlaylistItem::apply(Project& project) {
    PlaylistTrack* track = project.playlist.find(m_track);
    if (track == nullptr) {
        return;
    }
    const auto match = std::ranges::find(track->items, m_item, &PlaylistItem::id);
    m_removed = match != track->items.end();
    if (!m_removed) {
        return;
    }
    m_index = static_cast<std::size_t>(std::distance(track->items.begin(), match));
    m_previous = *match;
    track->items.erase(match);
}

void RemovePlaylistItem::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    PlaylistTrack* track = project.playlist.find(m_track);
    if (track == nullptr) {
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, track->items.size()));
    track->items.insert(track->items.begin() + at, m_previous);
}

std::string_view RemovePlaylistItem::name() const noexcept {
    return "Delete clip";
}

CommandId RemovePlaylistItem::kind() const noexcept {
    return CommandId::kRemovePlaylistItem;
}

DirtyMask RemovePlaylistItem::dirty() const noexcept {
    return dirty::kPlaylist;
}

// --- MovePlaylistItem --------------------------------------------------------

MovePlaylistItem::MovePlaylistItem(core::PlaylistTrackId track, core::ItemId item,
                                   core::Ticks start, core::PlaylistTrackId destination)
    : m_track(track), m_item(item), m_start(start), m_destination(destination) {}

void MovePlaylistItem::apply(Project& project) {
    PlaylistTrack* source = project.playlist.find(m_track);
    if (source == nullptr) {
        return;
    }
    const auto match = std::ranges::find(source->items, m_item, &PlaylistItem::id);
    if (match == source->items.end()) {
        m_moved = false;
        return;
    }
    m_previousStart = match->start;
    m_previousIndex = static_cast<std::size_t>(std::distance(source->items.begin(), match));
    match->start = m_start;

    if (!m_destination.valid() || m_destination == m_track) {
        m_moved = false;
        return;
    }
    PlaylistTrack* target = project.playlist.find(m_destination);
    if (target == nullptr) {
        m_moved = false;
        return;
    }
    const PlaylistItem moved = *match;
    source->items.erase(match);
    target->items.push_back(moved);
    m_moved = true;
}

void MovePlaylistItem::revert(Project& project) {
    PlaylistTrack* source = project.playlist.find(m_track);
    if (source == nullptr) {
        return;
    }
    if (m_moved) {
        PlaylistTrack* target = project.playlist.find(m_destination);
        if (target == nullptr) {
            return;
        }
        const auto match = std::ranges::find(target->items, m_item, &PlaylistItem::id);
        if (match == target->items.end()) {
            return;
        }
        PlaylistItem moved = *match;
        target->items.erase(match);
        moved.start = m_previousStart;
        const auto at =
            static_cast<std::ptrdiff_t>(std::min(m_previousIndex, source->items.size()));
        source->items.insert(source->items.begin() + at, moved);
        return;
    }
    const auto match = std::ranges::find(source->items, m_item, &PlaylistItem::id);
    if (match != source->items.end()) {
        match->start = m_previousStart;
    }
}

std::string_view MovePlaylistItem::name() const noexcept {
    return "Move clip";
}

CommandId MovePlaylistItem::kind() const noexcept {
    return CommandId::kMovePlaylistItem;
}

DirtyMask MovePlaylistItem::dirty() const noexcept {
    return dirty::kPlaylist;
}

std::uint64_t MovePlaylistItem::targetKey() const noexcept {
    return (static_cast<std::uint64_t>(m_track.value) << 32U) | m_item.value;
}

bool MovePlaylistItem::coalesceWith(const Command& next) {
    const auto& other = static_cast<const MovePlaylistItem&>(next);
    // A drag that crossed a track boundary is not merged: the revert path for a
    // cross-track move has to know which track the item was in when the move began,
    // and folding two of them together loses that.
    if (m_moved || other.m_moved) {
        return false;
    }
    m_start = other.m_start;
    return true;
}

} // namespace adx::project
