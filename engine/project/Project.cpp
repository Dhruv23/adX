// The aggregate's implementation, plus the lookups on the entity containers it
// owns. Those live here rather than in four one-function files because every one of
// them is a linear scan over a member of this class and nothing else uses them.
#include "engine/project/Project.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace adx::project {
namespace {

template<class Container, class Id>
[[nodiscard]] auto* findById(Container& container, Id id) noexcept {
    if (!id.valid()) {
        return decltype(&container[0]){nullptr};
    }
    const auto match =
        std::ranges::find(container, id, [](const auto& entity) { return entity.id; });
    return match == container.end() ? decltype(&container[0]){nullptr} : &*match;
}

} // namespace

// --- Pattern -----------------------------------------------------------------

const NoteClip* Pattern::clipFor(core::ChannelId channel) const noexcept {
    const auto match = std::ranges::find(noteClips, channel, &NoteClip::channel);
    return match == noteClips.end() ? nullptr : &*match;
}

NoteClip* Pattern::clipFor(core::ChannelId channel) noexcept {
    const auto match = std::ranges::find(noteClips, channel, &NoteClip::channel);
    return match == noteClips.end() ? nullptr : &*match;
}

const NoteExtras* NoteClip::extrasFor(core::NoteId note) const noexcept {
    const auto match = std::ranges::lower_bound(extras, note.value, {},
                                                [](const NoteExtras& e) { return e.note.value; });
    return match != extras.end() && match->note == note ? &*match : nullptr;
}

NoteExtras NoteClip::setExtras(NoteExtras value) {
    const auto match = std::ranges::lower_bound(extras, value.note.value, {},
                                                [](const NoteExtras& e) { return e.note.value; });
    NoteExtras previous{.note = value.note, .slide = {}, .pitchCurve = {}, .lyric = {}};
    if (match != extras.end() && match->note == value.note) {
        previous = std::move(*match);
        if (value.empty()) {
            extras.erase(match);
        } else {
            *match = std::move(value);
        }
    } else if (!value.empty()) {
        extras.insert(match, std::move(value));
    }
    return previous;
}

// --- Playlist ----------------------------------------------------------------

const char* toString(ClipTarget target) noexcept {
    switch (target) {
    case ClipTarget::Gain:
        return "gain";
    case ClipTarget::Pan:
        return "pan";
    case ClipTarget::PitchCents:
        return "pitch";
    case ClipTarget::Param:
        break;
    }
    return "";
}

bool clipTargetFromString(std::string_view name, ClipTarget& out) noexcept {
    if (name == "gain") {
        out = ClipTarget::Gain;
    } else if (name == "pan") {
        out = ClipTarget::Pan;
    } else if (name == "pitch") {
        out = ClipTarget::PitchCents;
    } else {
        return false;
    }
    return true;
}

const PlaylistTrack* Playlist::find(core::PlaylistTrackId id) const noexcept {
    return findById(tracks, id);
}

PlaylistTrack* Playlist::find(core::PlaylistTrackId id) noexcept {
    return findById(tracks, id);
}

// --- Mixer -------------------------------------------------------------------

const SlotParam* Slot::find(std::string_view name) const noexcept {
    const auto match = std::ranges::find(params, name, &SlotParam::name);
    return match == params.end() ? nullptr : &*match;
}

SlotParam* Slot::find(std::string_view name) noexcept {
    const auto match = std::ranges::find(params, name, &SlotParam::name);
    return match == params.end() ? nullptr : &*match;
}

const Insert* Mixer::find(core::InsertId id) const noexcept {
    return findById(inserts, id);
}

Insert* Mixer::find(core::InsertId id) noexcept {
    return findById(inserts, id);
}

const Slot* Mixer::findSlot(core::SlotId id) const noexcept {
    if (!id.valid()) {
        return nullptr;
    }
    for (const Insert& insert : inserts) {
        const auto match = std::ranges::find(insert.slots, id, &Slot::id);
        if (match != insert.slots.end()) {
            return &*match;
        }
    }
    return nullptr;
}

Slot* Mixer::findSlot(core::SlotId id) noexcept {
    // const_cast against our own non-const object: the const overload above is the
    // one implementation, and duplicating a nested loop to change one qualifier is
    // how the two drift apart.
    return const_cast<Slot*>(
        std::as_const(*this).findSlot(id)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

const Send* Mixer::findSend(core::SendId id) const noexcept {
    if (!id.valid()) {
        return nullptr;
    }
    for (const Insert& insert : inserts) {
        const auto match = std::ranges::find(insert.sends, id, &Send::id);
        if (match != insert.sends.end()) {
            return &*match;
        }
    }
    return nullptr;
}

Send* Mixer::findSend(core::SendId id) noexcept {
    return const_cast<Send*>(
        std::as_const(*this).findSend(id)); // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

core::InsertId Mixer::ownerOfSlot(core::SlotId id) const noexcept {
    for (const Insert& insert : inserts) {
        if (std::ranges::find(insert.slots, id, &Slot::id) != insert.slots.end()) {
            return insert.id;
        }
    }
    return core::InsertId{};
}

core::InsertId Mixer::ownerOfSend(core::SendId id) const noexcept {
    for (const Insert& insert : inserts) {
        if (std::ranges::find(insert.sends, id, &Send::id) != insert.sends.end()) {
            return insert.id;
        }
    }
    return core::InsertId{};
}

// --- Resources ---------------------------------------------------------------

const SampleRef* Resources::find(core::SampleId id) const noexcept {
    return findById(samples, id);
}

const SampleRef* Resources::findByPath(std::string_view path) const noexcept {
    const auto match = std::ranges::find(samples, path, &SampleRef::path);
    return match == samples.end() ? nullptr : &*match;
}

// --- Project -----------------------------------------------------------------

IdMarks Project::idMarks() const noexcept {
    return IdMarks{.channel = m_channelIds.mark(),
                   .pattern = m_patternIds.mark(),
                   .note = m_noteIds.mark(),
                   .insert = m_insertIds.mark(),
                   .slot = m_slotIds.mark(),
                   .send = m_sendIds.mark(),
                   .route = m_routeIds.mark(),
                   .playlistTrack = m_playlistTrackIds.mark(),
                   .item = m_itemIds.mark(),
                   .marker = m_markerIds.mark(),
                   .sample = m_sampleIds.mark(),
                   .automationClip = m_automationClipIds.mark()};
}

void Project::restoreIdMarks(const IdMarks& marks) noexcept {
    m_channelIds.restore(marks.channel);
    m_patternIds.restore(marks.pattern);
    m_noteIds.restore(marks.note);
    m_insertIds.restore(marks.insert);
    m_slotIds.restore(marks.slot);
    m_sendIds.restore(marks.send);
    m_routeIds.restore(marks.route);
    m_playlistTrackIds.restore(marks.playlistTrack);
    m_itemIds.restore(marks.item);
    m_markerIds.restore(marks.marker);
    m_sampleIds.restore(marks.sample);
    m_automationClipIds.restore(marks.automationClip);
}

const Channel* Project::find(core::ChannelId id) const noexcept {
    return findById(channels, id);
}

Channel* Project::find(core::ChannelId id) noexcept {
    return findById(channels, id);
}

const Pattern* Project::find(core::PatternId id) const noexcept {
    return findById(patterns, id);
}

Pattern* Project::find(core::PatternId id) noexcept {
    return findById(patterns, id);
}

const Marker* Project::find(core::MarkerId id) const noexcept {
    return findById(markers, id);
}

Marker* Project::find(core::MarkerId id) noexcept {
    return findById(markers, id);
}

const Channel* Project::findChannelByName(std::string_view name) const noexcept {
    const auto match = std::ranges::find(channels, name, &Channel::name);
    return match == channels.end() ? nullptr : &*match;
}

const Pattern* Project::findPatternByName(std::string_view name) const noexcept {
    const auto match = std::ranges::find(patterns, name, &Pattern::name);
    return match == patterns.end() ? nullptr : &*match;
}

const AutomationClip* Project::findAutomationClip(core::AutomationClipId id) const noexcept {
    if (!id.valid()) {
        return nullptr;
    }
    for (const Pattern& pattern : patterns) {
        const auto match = std::ranges::find(pattern.autoClips, id, &AutomationClip::id);
        if (match != pattern.autoClips.end()) {
            return &*match;
        }
    }
    const auto match = std::ranges::find(playlist.autoClips, id, &AutomationClip::id);
    return match == playlist.autoClips.end() ? nullptr : &*match;
}

AutomationClip* Project::findAutomationClip(core::AutomationClipId id) noexcept {
    return const_cast<AutomationClip*>( // NOLINT(cppcoreguidelines-pro-type-const-cast)
        std::as_const(*this).findAutomationClip(id));
}

core::Ticks Project::contentLength() const noexcept {
    core::Ticks longest{0};
    for (const PlaylistTrack& track : playlist.tracks) {
        for (const PlaylistItem& item : track.items) {
            core::Ticks length = item.length;
            if (length.value <= 0) {
                if (const auto* ref = std::get_if<PatternRef>(&item.content)) {
                    const Pattern* pattern = find(ref->pattern);
                    length = pattern != nullptr ? pattern->length : core::Ticks{0};
                } else {
                    // An audio clip's natural length needs the file, which Phase 4
                    // loads. Until then an untrimmed audio item contributes nothing
                    // to the project's length rather than guessing at one.
                    length = core::Ticks{0};
                }
            }
            longest = std::max(longest, item.start + length);
        }
    }
    return longest;
}

} // namespace adx::project
