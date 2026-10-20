#include "engine/project/commands/ArrangeCommands.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace adx::project {
namespace {

/// Moves element `from` of `items` to position `to` (both in range), keeping the
/// order of everything else.
template<class T> void moveElement(std::vector<T>& items, std::size_t from, std::size_t to) {
    if (from == to) {
        return;
    }
    T moving = std::move(items[from]);
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(from));
    items.insert(items.begin() + static_cast<std::ptrdiff_t>(to), std::move(moving));
}

/// The track holding item `id`, and the item's position in it, or a null track.
std::pair<PlaylistTrack*, std::size_t> locateItem(Project& project, core::ItemId id) noexcept {
    for (PlaylistTrack& track : project.playlist.tracks) {
        const auto match = std::ranges::find(track.items, id, &PlaylistItem::id);
        if (match != track.items.end()) {
            return {&track, static_cast<std::size_t>(std::distance(track.items.begin(), match))};
        }
    }
    return {nullptr, 0};
}

} // namespace

// --- MoveSlot ----------------------------------------------------------------

MoveSlot::MoveSlot(core::SlotId slot, std::size_t index) : m_slot(slot), m_index(index) {}

void MoveSlot::apply(Project& project) {
    m_insert = project.mixer.ownerOfSlot(m_slot);
    Insert* insert = project.mixer.find(m_insert);
    m_moved = false;
    if (insert == nullptr || insert->slots.empty()) {
        return;
    }
    const auto match = std::ranges::find(insert->slots, m_slot, &Slot::id);
    m_previousIndex = static_cast<std::size_t>(std::distance(insert->slots.begin(), match));
    const std::size_t to = std::min(m_index, insert->slots.size() - 1);
    moveElement(insert->slots, m_previousIndex, to);
    m_moved = true;
}

void MoveSlot::revert(Project& project) {
    Insert* insert = project.mixer.find(m_insert);
    if (!m_moved || insert == nullptr) {
        return;
    }
    const auto match = std::ranges::find(insert->slots, m_slot, &Slot::id);
    if (match == insert->slots.end()) {
        return;
    }
    moveElement(insert->slots,
                static_cast<std::size_t>(std::distance(insert->slots.begin(), match)),
                m_previousIndex);
}

std::string_view MoveSlot::name() const noexcept {
    return "Move effect";
}

CommandId MoveSlot::kind() const noexcept {
    return CommandId::kMoveSlot;
}

DirtyMask MoveSlot::dirty() const noexcept {
    return dirty::kMixer;
}

// --- SetPlaylistItem ---------------------------------------------------------

SetPlaylistItem::SetPlaylistItem(core::PlaylistTrackId track, core::ItemId item, PlaylistItem value,
                                 core::PlaylistTrackId destination)
    : m_track(track), m_item(item), m_value(std::move(value)), m_destination(destination) {}

void SetPlaylistItem::apply(Project& project) {
    m_previous.reset();
    PlaylistTrack* source = project.playlist.find(m_track);
    if (source == nullptr) {
        return;
    }
    const auto match = std::ranges::find(source->items, m_item, &PlaylistItem::id);
    if (match == source->items.end()) {
        return;
    }
    m_previous = *match;
    m_previousIndex = static_cast<std::size_t>(std::distance(source->items.begin(), match));
    PlaylistItem updated = m_value;
    updated.id = m_item;

    PlaylistTrack* target = project.playlist.find(m_destination);
    if (target == nullptr || target == source) {
        *match = std::move(updated);
        m_landedOn = m_track;
        return;
    }
    source->items.erase(match);
    target->items.push_back(std::move(updated));
    m_landedOn = m_destination;
}

void SetPlaylistItem::revert(Project& project) {
    if (!m_previous) {
        return;
    }
    // Wherever the item is now - a coalesced drag may have crossed several tracks.
    const auto [current, index] = locateItem(project, m_item);
    if (current != nullptr) {
        current->items.erase(current->items.begin() + static_cast<std::ptrdiff_t>(index));
    }
    PlaylistTrack* source = project.playlist.find(m_track);
    if (source == nullptr) {
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_previousIndex, source->items.size()));
    source->items.insert(source->items.begin() + at, *m_previous);
}

std::string_view SetPlaylistItem::name() const noexcept {
    return "Edit clip";
}

CommandId SetPlaylistItem::kind() const noexcept {
    return CommandId::kSetPlaylistItem;
}

DirtyMask SetPlaylistItem::dirty() const noexcept {
    return dirty::kPlaylist;
}

std::uint64_t SetPlaylistItem::targetKey() const noexcept {
    // Item ids are project-wide, so the item alone names the target - which lets a drag
    // that crosses tracks stay one entry.
    return m_item.value;
}

bool SetPlaylistItem::coalesceWith(const Command& next) {
    const auto& other = static_cast<const SetPlaylistItem&>(next);
    if (!m_previous || !other.m_previous) {
        return false;
    }
    m_value = other.m_value;
    m_destination = other.m_destination;
    m_landedOn = other.m_landedOn;
    return true;
}

// --- SetPlaylistTrack --------------------------------------------------------

SetPlaylistTrack::SetPlaylistTrack(core::PlaylistTrackId track, PlaylistTrack value)
    : m_track(track), m_value(std::move(value)) {}

void SetPlaylistTrack::apply(Project& project) {
    m_previous.reset();
    PlaylistTrack* track = project.playlist.find(m_track);
    if (track == nullptr) {
        return;
    }
    m_previous = *track;
    m_previous->items.clear();
    track->name = m_value.name;
    track->color = m_value.color;
    track->height = std::clamp<std::uint16_t>(m_value.height, 1, 64);
    track->muted = m_value.muted;
    track->soloed = m_value.soloed;
    // A track cannot be its own group, nor join a group that is itself a member.
    const PlaylistTrack* group = project.playlist.find(m_value.group);
    track->group = group != nullptr && group->id != m_track && !group->group.valid()
                       ? m_value.group
                       : core::PlaylistTrackId{};
    track->collapsed = m_value.collapsed;
}

void SetPlaylistTrack::revert(Project& project) {
    PlaylistTrack* track = project.playlist.find(m_track);
    if (!m_previous || track == nullptr) {
        return;
    }
    std::vector<PlaylistItem> items = std::move(track->items);
    *track = *m_previous;
    track->items = std::move(items);
}

std::string_view SetPlaylistTrack::name() const noexcept {
    return "Edit track";
}

CommandId SetPlaylistTrack::kind() const noexcept {
    return CommandId::kSetPlaylistTrack;
}

DirtyMask SetPlaylistTrack::dirty() const noexcept {
    return dirty::kPlaylist;
}

std::uint64_t SetPlaylistTrack::targetKey() const noexcept {
    return m_track.value;
}

bool SetPlaylistTrack::coalesceWith(const Command& next) {
    const auto& other = static_cast<const SetPlaylistTrack&>(next);
    if (!m_previous || !other.m_previous) {
        return false;
    }
    m_value = other.m_value;
    return true;
}

// --- MovePlaylistTrack -------------------------------------------------------

MovePlaylistTrack::MovePlaylistTrack(core::PlaylistTrackId track, std::size_t index)
    : m_track(track), m_index(index) {}

void MovePlaylistTrack::apply(Project& project) {
    auto& tracks = project.playlist.tracks;
    const auto match = std::ranges::find(tracks, m_track, &PlaylistTrack::id);
    m_moved = match != tracks.end();
    if (!m_moved) {
        return;
    }
    m_previousIndex = static_cast<std::size_t>(std::distance(tracks.begin(), match));
    moveElement(tracks, m_previousIndex, std::min(m_index, tracks.size() - 1));
}

void MovePlaylistTrack::revert(Project& project) {
    auto& tracks = project.playlist.tracks;
    const auto match = std::ranges::find(tracks, m_track, &PlaylistTrack::id);
    if (!m_moved || match == tracks.end()) {
        return;
    }
    moveElement(tracks, static_cast<std::size_t>(std::distance(tracks.begin(), match)),
                m_previousIndex);
}

std::string_view MovePlaylistTrack::name() const noexcept {
    return "Move track";
}

CommandId MovePlaylistTrack::kind() const noexcept {
    return CommandId::kMovePlaylistTrack;
}

DirtyMask MovePlaylistTrack::dirty() const noexcept {
    return dirty::kPlaylist;
}

// --- arrangements ------------------------------------------------------------

AddArrangement::AddArrangement(std::string arrangementName, bool copyLive)
    : m_name(std::move(arrangementName)), m_copy(copyLive) {}

void AddArrangement::apply(Project& project) {
    Arrangement arrangement{.name = m_name, .tracks = {}};
    if (m_copy) {
        arrangement.tracks = project.playlist.tracks;
    }
    m_index = project.playlist.arrangements.size();
    project.playlist.arrangements.push_back(std::move(arrangement));
}

void AddArrangement::revert(Project& project) {
    auto& stored = project.playlist.arrangements;
    if (m_index < stored.size()) {
        stored.erase(stored.begin() + static_cast<std::ptrdiff_t>(m_index));
    }
}

std::string_view AddArrangement::name() const noexcept {
    return "Add arrangement";
}

CommandId AddArrangement::kind() const noexcept {
    return CommandId::kAddArrangement;
}

DirtyMask AddArrangement::dirty() const noexcept {
    return dirty::kPlaylist;
}

RemoveArrangement::RemoveArrangement(std::size_t index) : m_index(index) {}

void RemoveArrangement::apply(Project& project) {
    auto& stored = project.playlist.arrangements;
    m_removed.reset();
    if (m_index >= stored.size()) {
        return;
    }
    m_removed = std::move(stored[m_index]);
    stored.erase(stored.begin() + static_cast<std::ptrdiff_t>(m_index));
}

void RemoveArrangement::revert(Project& project) {
    auto& stored = project.playlist.arrangements;
    if (!m_removed) {
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, stored.size()));
    stored.insert(stored.begin() + at, *m_removed);
}

std::string_view RemoveArrangement::name() const noexcept {
    return "Remove arrangement";
}

CommandId RemoveArrangement::kind() const noexcept {
    return CommandId::kRemoveArrangement;
}

DirtyMask RemoveArrangement::dirty() const noexcept {
    return dirty::kPlaylist;
}

SwitchArrangement::SwitchArrangement(std::size_t index) : m_index(index) {}

void SwitchArrangement::swap(Project& project) const {
    Playlist& playlist = project.playlist;
    if (m_index >= playlist.arrangements.size()) {
        return;
    }
    Arrangement& stored = playlist.arrangements[m_index];
    std::swap(playlist.tracks, stored.tracks);
    std::swap(playlist.arrangementName, stored.name);
}

void SwitchArrangement::apply(Project& project) {
    swap(project);
}

void SwitchArrangement::revert(Project& project) {
    swap(project);
}

std::string_view SwitchArrangement::name() const noexcept {
    return "Switch arrangement";
}

CommandId SwitchArrangement::kind() const noexcept {
    return CommandId::kSwitchArrangement;
}

DirtyMask SwitchArrangement::dirty() const noexcept {
    return dirty::kPlaylist;
}

RenameArrangement::RenameArrangement(std::optional<std::size_t> index, std::string arrangementName)
    : m_index(index), m_name(std::move(arrangementName)) {}

std::string* RenameArrangement::target(Project& project) const noexcept {
    if (!m_index) {
        return &project.playlist.arrangementName;
    }
    auto& stored = project.playlist.arrangements;
    return *m_index < stored.size() ? &stored[*m_index].name : nullptr;
}

void RenameArrangement::apply(Project& project) {
    if (std::string* name = target(project)) {
        m_previous = std::exchange(*name, m_name);
    }
}

void RenameArrangement::revert(Project& project) {
    if (std::string* name = target(project)) {
        *name = m_previous;
    }
}

std::string_view RenameArrangement::name() const noexcept {
    return "Rename arrangement";
}

CommandId RenameArrangement::kind() const noexcept {
    return CommandId::kRenameArrangement;
}

DirtyMask RenameArrangement::dirty() const noexcept {
    return dirty::kPlaylist;
}

// --- controllers -------------------------------------------------------------

namespace {

/// A controller's dirty mask: a peak controller is a graph node, and every kind is an
/// automation lane, which the snapshot rebuilds from the routing it changes.
constexpr DirtyMask kControllerDirty = dirty::kMixer | dirty::kRouting;

} // namespace

AddController::AddController(Controller prototype) : m_prototype(std::move(prototype)) {}

void AddController::apply(Project& project) {
    m_marks = project.idMarks();
    m_id = project.newControllerId(m_prototype.id);
    Controller controller = m_prototype;
    controller.id = m_id;
    project.controllers.push_back(std::move(controller));
}

void AddController::revert(Project& project) {
    std::erase_if(project.controllers, [this](const Controller& c) { return c.id == m_id; });
    project.restoreIdMarks(m_marks);
    m_id = core::ControllerId{};
}

std::string_view AddController::name() const noexcept {
    return "Add controller";
}

CommandId AddController::kind() const noexcept {
    return CommandId::kAddController;
}

DirtyMask AddController::dirty() const noexcept {
    return kControllerDirty;
}

RemoveController::RemoveController(core::ControllerId id) : m_id(id) {}

void RemoveController::apply(Project& project) {
    auto& controllers = project.controllers;
    const auto match = std::ranges::find(controllers, m_id, &Controller::id);
    m_removed.reset();
    if (match == controllers.end()) {
        return;
    }
    m_index = static_cast<std::size_t>(std::distance(controllers.begin(), match));
    m_removed = std::move(*match);
    controllers.erase(match);
}

void RemoveController::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    auto& controllers = project.controllers;
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, controllers.size()));
    controllers.insert(controllers.begin() + at, *m_removed);
}

std::string_view RemoveController::name() const noexcept {
    return "Remove controller";
}

CommandId RemoveController::kind() const noexcept {
    return CommandId::kRemoveController;
}

DirtyMask RemoveController::dirty() const noexcept {
    return kControllerDirty;
}

SetController::SetController(core::ControllerId id, Controller value)
    : m_id(id), m_value(std::move(value)) {}

void SetController::apply(Project& project) {
    m_previous.reset();
    Controller* controller = project.find(m_id);
    if (controller == nullptr) {
        return;
    }
    m_previous = *controller;
    *controller = m_value;
    controller->id = m_id;
}

void SetController::revert(Project& project) {
    Controller* controller = project.find(m_id);
    if (m_previous && controller != nullptr) {
        *controller = *m_previous;
    }
}

std::string_view SetController::name() const noexcept {
    return "Edit controller";
}

CommandId SetController::kind() const noexcept {
    return CommandId::kSetController;
}

DirtyMask SetController::dirty() const noexcept {
    return kControllerDirty;
}

std::uint64_t SetController::targetKey() const noexcept {
    return m_id.value;
}

bool SetController::coalesceWith(const Command& next) {
    const auto& other = static_cast<const SetController&>(next);
    if (!m_previous || !other.m_previous) {
        return false;
    }
    m_value = other.m_value;
    return true;
}

// --- whole instruments and effects ----------------------------------------------

SetInstrument::SetInstrument(core::ChannelId channel, InstrumentSpec spec)
    : m_channel(channel), m_spec(std::move(spec)) {}

void SetInstrument::apply(Project& project) {
    m_previous.reset();
    Channel* channel = project.find(m_channel);
    if (channel == nullptr) {
        return;
    }
    m_previous = std::exchange(channel->instrument, m_spec);
}

void SetInstrument::revert(Project& project) {
    Channel* channel = project.find(m_channel);
    if (m_previous && channel != nullptr) {
        channel->instrument = *m_previous;
    }
}

std::string_view SetInstrument::name() const noexcept {
    return "Apply preset";
}

CommandId SetInstrument::kind() const noexcept {
    return CommandId::kSetInstrument;
}

DirtyMask SetInstrument::dirty() const noexcept {
    return dirty::kChannels;
}

SetSlotEffect::SetSlotEffect(core::SlotId slot, std::string type, std::vector<SlotParam> params,
                             float mix)
    : m_slot(slot), m_type(std::move(type)), m_params(std::move(params)), m_mix(mix) {}

void SetSlotEffect::apply(Project& project) {
    m_previous.reset();
    Slot* slot = project.mixer.findSlot(m_slot);
    if (slot == nullptr) {
        return;
    }
    m_previous = *slot;
    slot->type = m_type;
    slot->params = m_params;
    slot->mix = m_mix;
}

void SetSlotEffect::revert(Project& project) {
    Slot* slot = project.mixer.findSlot(m_slot);
    if (m_previous && slot != nullptr) {
        *slot = *m_previous;
    }
}

std::string_view SetSlotEffect::name() const noexcept {
    return "Set effect";
}

CommandId SetSlotEffect::kind() const noexcept {
    return CommandId::kSetSlotEffect;
}

DirtyMask SetSlotEffect::dirty() const noexcept {
    return dirty::kMixer;
}

SetSlotImpulse::SetSlotImpulse(core::SlotId slot, core::SampleId impulse)
    : m_slot(slot), m_impulse(impulse) {}

void SetSlotImpulse::apply(Project& project) {
    if (Slot* slot = project.mixer.findSlot(m_slot)) {
        m_previous = std::exchange(slot->impulse, m_impulse);
    }
}

void SetSlotImpulse::revert(Project& project) {
    if (Slot* slot = project.mixer.findSlot(m_slot)) {
        slot->impulse = m_previous;
    }
}

std::string_view SetSlotImpulse::name() const noexcept {
    return "Set impulse response";
}

CommandId SetSlotImpulse::kind() const noexcept {
    return CommandId::kSetSlotImpulse;
}

DirtyMask SetSlotImpulse::dirty() const noexcept {
    return dirty::kMixer | dirty::kResources;
}

} // namespace adx::project
