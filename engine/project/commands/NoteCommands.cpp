#include "engine/project/commands/NoteCommands.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace adx::project {
namespace {

[[nodiscard]] NoteClip* clipIn(Project& project, core::PatternId patternId,
                               core::ChannelId channelId) noexcept {
    Pattern* pattern = project.find(patternId);
    return pattern == nullptr ? nullptr : pattern->clipFor(channelId);
}

[[nodiscard]] double readField(const Note& note, NoteField field) noexcept {
    switch (field) {
    case NoteField::Start:
        return static_cast<double>(note.start.value);
    case NoteField::Length:
        return static_cast<double>(note.length.value);
    case NoteField::Pitch:
        return note.pitch;
    case NoteField::Velocity:
        return note.velocity;
    case NoteField::FineTuneCents:
        return note.fineTuneCents;
    case NoteField::ReleaseVelocity:
        return note.releaseVelocity;
    case NoteField::Pan:
        return note.pan;
    case NoteField::Cutoff:
        return note.cutoff;
    case NoteField::Resonance:
        return note.resonance;
    }
    return 0.0;
}

void writeField(Note& note, NoteField field, double value) noexcept {
    switch (field) {
    case NoteField::Start:
        note.start = core::Ticks{static_cast<std::int64_t>(value)};
        break;
    case NoteField::Length:
        note.length = core::Ticks{std::max<std::int64_t>(1, static_cast<std::int64_t>(value))};
        break;
    case NoteField::Pitch:
        note.pitch = static_cast<std::uint8_t>(std::clamp(value, 0.0, 127.0));
        break;
    case NoteField::Velocity:
        note.velocity = static_cast<std::uint8_t>(std::clamp(value, 0.0, 127.0));
        break;
    case NoteField::FineTuneCents:
        note.fineTuneCents = static_cast<std::int16_t>(std::clamp(value, -32768.0, 32767.0));
        break;
    case NoteField::ReleaseVelocity:
        note.releaseVelocity = static_cast<std::uint16_t>(std::clamp(value, 0.0, 127.0));
        break;
    case NoteField::Pan:
        note.pan = static_cast<float>(std::clamp(value, -1.0, 1.0));
        break;
    case NoteField::Cutoff:
        note.cutoff = static_cast<float>(value);
        break;
    case NoteField::Resonance:
        note.resonance = static_cast<float>(value);
        break;
    }
}

} // namespace

// --- AddNotes ----------------------------------------------------------------

AddNotes::AddNotes(core::PatternId pattern, core::ChannelId channel, std::vector<Note> notes,
                   std::vector<NoteExtrasAt> extras)
    : m_pattern(pattern), m_channel(channel), m_notes(std::move(notes)),
      m_extras(std::move(extras)) {}

void AddNotes::apply(Project& project) {
    Pattern* pattern = project.find(m_pattern);
    if (pattern == nullptr) {
        return;
    }
    m_marks = project.idMarks();
    NoteClip* clip = pattern->clipFor(m_channel);
    m_createdClip = clip == nullptr;
    if (m_createdClip) {
        pattern->noteClips.push_back(NoteClip{.channel = m_channel, .notes = {}});
        clip = &pattern->noteClips.back();
    }

    m_created.clear();
    m_created.reserve(m_notes.size());
    clip->notes.reserve(clip->notes.size() + m_notes.size());
    for (Note note : m_notes) {
        note.id = project.newNoteId(note.id);
        m_created.push_back(note.id);
        clip->notes.push_back(note);
    }
    for (const NoteExtrasAt& at : m_extras) {
        if (at.index < m_created.size() && !at.extras.empty()) {
            NoteExtras extras = at.extras;
            extras.note = m_created[at.index];
            static_cast<void>(clip->setExtras(std::move(extras)));
        }
    }
}

void AddNotes::revert(Project& project) {
    // Nothing was allocated if apply() could not find the pattern, and restoring
    // zeroed marks would reset every id counter in the project.
    if (m_created.empty()) {
        return;
    }
    Pattern* pattern = project.find(m_pattern);
    if (pattern == nullptr) {
        return;
    }
    if (NoteClip* clip = pattern->clipFor(m_channel)) {
        std::erase_if(clip->notes, [this](const Note& note) {
            return std::ranges::find(m_created, note.id) != m_created.end();
        });
        std::erase_if(clip->extras, [this](const NoteExtras& extras) {
            return std::ranges::find(m_created, extras.note) != m_created.end();
        });
        if (m_createdClip && clip->notes.empty()) {
            std::erase_if(pattern->noteClips,
                          [this](const NoteClip& c) { return c.channel == m_channel; });
        }
    }
    project.restoreIdMarks(m_marks);
    m_created.clear();
}

std::string_view AddNotes::name() const noexcept {
    return "Add notes";
}

CommandId AddNotes::kind() const noexcept {
    return CommandId::kAddNotes;
}

DirtyMask AddNotes::dirty() const noexcept {
    return dirty::kPatterns;
}

// --- RemoveNotes -------------------------------------------------------------

RemoveNotes::RemoveNotes(core::PatternId pattern, core::ChannelId channel,
                         std::vector<core::NoteId> ids)
    : m_pattern(pattern), m_channel(channel), m_ids(std::move(ids)) {}

void RemoveNotes::apply(Project& project) {
    NoteClip* clip = clipIn(project, m_pattern, m_channel);
    if (clip == nullptr) {
        return;
    }
    m_removed.clear();
    // Descending, so each erase leaves the indices of the ones still to come intact.
    // revert() walks the list backwards - ascending - which is the only order in
    // which inserting at the recorded index reproduces the original.
    for (std::size_t i = clip->notes.size(); i-- > 0;) {
        if (std::ranges::find(m_ids, clip->notes[i].id) == m_ids.end()) {
            continue;
        }
        const core::NoteId id = clip->notes[i].id;
        m_removed.push_back(Detached{.index = i,
                                     .note = clip->notes[i],
                                     .extras = clip->setExtras(NoteExtras{.note = id})});
        clip->notes.erase(clip->notes.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void RemoveNotes::revert(Project& project) {
    NoteClip* clip = clipIn(project, m_pattern, m_channel);
    if (clip == nullptr) {
        return;
    }
    for (const Detached& detached : std::views::reverse(m_removed)) {
        const auto at = static_cast<std::ptrdiff_t>(std::min(detached.index, clip->notes.size()));
        clip->notes.insert(clip->notes.begin() + at, detached.note);
        static_cast<void>(clip->setExtras(detached.extras));
    }
    m_removed.clear();
}

std::string_view RemoveNotes::name() const noexcept {
    return "Delete notes";
}

CommandId RemoveNotes::kind() const noexcept {
    return CommandId::kRemoveNotes;
}

DirtyMask RemoveNotes::dirty() const noexcept {
    return dirty::kPatterns;
}

// --- MoveNotes ---------------------------------------------------------------

MoveNotes::MoveNotes(core::PatternId pattern, core::ChannelId channel,
                     std::vector<core::NoteId> ids, core::Ticks deltaTicks, int deltaPitch)
    : m_pattern(pattern), m_channel(channel), m_ids(std::move(ids)), m_deltaTicks(deltaTicks),
      m_deltaPitch(deltaPitch) {}

void MoveNotes::apply(Project& project) {
    NoteClip* clip = clipIn(project, m_pattern, m_channel);
    if (clip == nullptr) {
        return;
    }
    m_applied.clear();
    m_applied.reserve(m_ids.size());
    for (const core::NoteId id : m_ids) {
        const auto match = std::ranges::find(clip->notes, id, &Note::id);
        if (match == clip->notes.end()) {
            m_applied.emplace_back(core::Ticks{0}, 0);
            continue;
        }
        const core::Ticks newStart{
            std::max<std::int64_t>(0, match->start.value + m_deltaTicks.value)};
        const int newPitch = std::clamp(static_cast<int>(match->pitch) + m_deltaPitch, 0, 127);
        m_applied.emplace_back(newStart - match->start, newPitch - static_cast<int>(match->pitch));
        match->start = newStart;
        match->pitch = static_cast<std::uint8_t>(newPitch);
    }
}

void MoveNotes::revert(Project& project) {
    NoteClip* clip = clipIn(project, m_pattern, m_channel);
    if (clip == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < m_ids.size() && i < m_applied.size(); ++i) {
        const auto match = std::ranges::find(clip->notes, m_ids[i], &Note::id);
        if (match == clip->notes.end()) {
            continue;
        }
        match->start = match->start - m_applied[i].first;
        match->pitch =
            static_cast<std::uint8_t>(static_cast<int>(match->pitch) - m_applied[i].second);
    }
}

std::string_view MoveNotes::name() const noexcept {
    return "Move notes";
}

CommandId MoveNotes::kind() const noexcept {
    return CommandId::kMoveNotes;
}

DirtyMask MoveNotes::dirty() const noexcept {
    return dirty::kPatterns;
}

std::uint64_t MoveNotes::targetKey() const noexcept {
    // The selection, not the pattern: dragging one note and then a different one
    // within the window are two edits. The first id is a good enough proxy for the
    // selection because a drag never changes what is selected mid-drag.
    const std::uint64_t first = m_ids.empty() ? 0 : m_ids.front().value;
    return (static_cast<std::uint64_t>(m_pattern.value) << 32U) ^ (first * 31U) ^ m_ids.size();
}

bool MoveNotes::coalesceWith(const Command& next) {
    const auto& other = static_cast<const MoveNotes&>(next);
    if (other.m_ids != m_ids || other.m_channel != m_channel) {
        return false;
    }
    // Accumulate what was actually applied, not what was asked for: two moves that
    // were each clamped must undo to the original position, not to the position the
    // unclamped deltas imply.
    for (std::size_t i = 0; i < m_applied.size() && i < other.m_applied.size(); ++i) {
        m_applied[i].first += other.m_applied[i].first;
        m_applied[i].second += other.m_applied[i].second;
    }
    m_deltaTicks += other.m_deltaTicks;
    m_deltaPitch += other.m_deltaPitch;
    return true;
}

// --- SetNoteValue ------------------------------------------------------------

SetNoteValue::SetNoteValue(core::PatternId pattern, core::ChannelId channel,
                           std::vector<core::NoteId> ids, NoteField field, double value)
    : m_pattern(pattern), m_channel(channel), m_ids(std::move(ids)), m_field(field),
      m_value(value) {}

void SetNoteValue::apply(Project& project) {
    NoteClip* clip = clipIn(project, m_pattern, m_channel);
    if (clip == nullptr) {
        return;
    }
    m_previous.assign(m_ids.size(), 0.0);
    for (std::size_t i = 0; i < m_ids.size(); ++i) {
        const auto match = std::ranges::find(clip->notes, m_ids[i], &Note::id);
        if (match == clip->notes.end()) {
            continue;
        }
        m_previous[i] = readField(*match, m_field);
        writeField(*match, m_field, m_value);
    }
}

void SetNoteValue::revert(Project& project) {
    NoteClip* clip = clipIn(project, m_pattern, m_channel);
    if (clip == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < m_ids.size() && i < m_previous.size(); ++i) {
        const auto match = std::ranges::find(clip->notes, m_ids[i], &Note::id);
        if (match != clip->notes.end()) {
            writeField(*match, m_field, m_previous[i]);
        }
    }
}

std::string_view SetNoteValue::name() const noexcept {
    return "Set note value";
}

CommandId SetNoteValue::kind() const noexcept {
    return CommandId::kSetNoteValue;
}

DirtyMask SetNoteValue::dirty() const noexcept {
    return dirty::kPatterns;
}

std::uint64_t SetNoteValue::targetKey() const noexcept {
    const std::uint64_t first = m_ids.empty() ? 0 : m_ids.front().value;
    return (static_cast<std::uint64_t>(m_field) << 48U) ^
           (static_cast<std::uint64_t>(m_pattern.value) << 32U) ^ (first * 31U) ^ m_ids.size();
}

bool SetNoteValue::coalesceWith(const Command& next) {
    const auto& other = static_cast<const SetNoteValue&>(next);
    if (other.m_ids != m_ids || other.m_field != m_field || other.m_channel != m_channel) {
        return false;
    }
    m_value = other.m_value;
    return true;
}



// --- SetNoteExtra ------------------------------------------------------------

SetNoteExtra::SetNoteExtra(core::PatternId pattern, core::ChannelId channel, core::NoteId note,
                           NoteExtra part, NoteExtras value)
    : m_pattern(pattern), m_channel(channel), m_note(note), m_part(part),
      m_value(std::move(value)) {}

void SetNoteExtra::apply(Project& project) {
    NoteClip* clip = clipIn(project, m_pattern, m_channel);
    if (clip == nullptr ||
        std::ranges::find(clip->notes, m_note, &Note::id) == clip->notes.end()) {
        return;
    }
    const NoteExtras* existing = clip->extrasFor(m_note);
    NoteExtras next = existing != nullptr ? *existing : NoteExtras{.note = m_note};
    switch (m_part) {
    case NoteExtra::Slide:
        next.slide = m_value.slide;
        break;
    case NoteExtra::PitchCurve:
        next.pitchCurve = m_value.pitchCurve;
        std::ranges::stable_sort(next.pitchCurve, {}, [](const PitchPoint& p) { return p.at; });
        break;
    case NoteExtra::Lyric:
        next.lyric = m_value.lyric;
        break;
    }
    m_previous = clip->setExtras(std::move(next));
}

void SetNoteExtra::revert(Project& project) {
    NoteClip* clip = clipIn(project, m_pattern, m_channel);
    if (clip == nullptr || !m_previous.has_value()) {
        return;
    }
    static_cast<void>(clip->setExtras(std::move(*m_previous)));
    m_previous.reset();
}

DirtyMask SetNoteExtra::dirty() const noexcept {
    return dirty::kPatterns;
}

SetNoteSlide::SetNoteSlide(core::PatternId pattern, core::ChannelId channel, core::NoteId note,
                           std::optional<NoteSlide> slide)
    : SetNoteExtra(pattern, channel, note, NoteExtra::Slide,
                   NoteExtras{.note = note, .slide = slide, .pitchCurve = {}, .lyric = {}}) {}

std::string_view SetNoteSlide::name() const noexcept {
    return "Set slide";
}

CommandId SetNoteSlide::kind() const noexcept {
    return CommandId::kSetNoteSlide;
}

SetPitchCurve::SetPitchCurve(core::PatternId pattern, core::ChannelId channel,
                             core::NoteId note, std::vector<PitchPoint> curve)
    : SetNoteExtra(pattern, channel, note, NoteExtra::PitchCurve,
                   NoteExtras{.note = note, .slide = {}, .pitchCurve = std::move(curve),
                              .lyric = {}}) {}

std::string_view SetPitchCurve::name() const noexcept {
    return "Set pitch curve";
}

CommandId SetPitchCurve::kind() const noexcept {
    return CommandId::kSetPitchCurve;
}

SetLyric::SetLyric(core::PatternId pattern, core::ChannelId channel, core::NoteId note,
                   std::string lyric)
    : SetNoteExtra(pattern, channel, note, NoteExtra::Lyric,
                   NoteExtras{.note = note, .slide = {}, .pitchCurve = {},
                              .lyric = std::move(lyric)}) {}

std::string_view SetLyric::name() const noexcept {
    return "Set lyric";
}

CommandId SetLyric::kind() const noexcept {
    return CommandId::kSetLyric;
}

} // namespace adx::project
