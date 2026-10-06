#include "engine/project/commands/EditNotes.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace adx::project {
namespace {

[[nodiscard]] Note sanitised(Note note) noexcept {
    note.length = core::Ticks{std::max<std::int64_t>(1, note.length.value)};
    note.start = core::Ticks{std::max<std::int64_t>(0, note.start.value)};
    note.pitch = std::min<std::uint8_t>(note.pitch, 127);
    note.velocity = std::min<std::uint8_t>(note.velocity, 127);
    return note;
}

} // namespace

EditNotes::EditNotes(core::PatternId pattern, core::ChannelId channel,
                     std::vector<core::NoteId> remove, std::vector<Note> update,
                     std::vector<Note> add, std::string label)
    : m_pattern(pattern), m_channel(channel), m_remove(std::move(remove)),
      m_update(std::move(update)), m_add(std::move(add)), m_label(std::move(label)) {}

void EditNotes::apply(Project& project) {
    Pattern* pattern = project.find(m_pattern);
    if (pattern == nullptr) {
        return;
    }
    m_previous.clear();
    m_removed.clear();
    m_created.clear();
    m_createdClip = false;
    m_removedClip = false;

    NoteClip* clip = pattern->clipFor(m_channel);
    if (clip == nullptr) {
        if (m_add.empty()) {
            return;
        }
        pattern->noteClips.push_back(NoteClip{.channel = m_channel, .notes = {}, .extras = {}});
        clip = &pattern->noteClips.back();
        m_createdClip = true;
    }
    applyUpdates(*clip);
    applyRemovals(*clip);
    applyAdditions(project, *clip);

    if (clip->notes.empty()) {
        const auto at = std::ranges::find(pattern->noteClips, m_channel, &NoteClip::channel);
        m_removedClipIndex = static_cast<std::size_t>(at - pattern->noteClips.begin());
        pattern->noteClips.erase(at);
        m_removedClip = !m_createdClip;
    }
}

void EditNotes::applyUpdates(NoteClip& clip) {
    for (const Note& wanted : m_update) {
        const auto it = std::ranges::find(clip.notes, wanted.id, &Note::id);
        if (it == clip.notes.end()) {
            continue;
        }
        m_previous.push_back(*it);
        *it = sanitised(wanted);
    }
}

void EditNotes::applyRemovals(NoteClip& clip) {
    if (m_remove.empty()) {
        return;
    }
    // Descending, so each erase leaves the indices still to come intact; revert()
    // walks them ascending, the only order in which re-inserting at the recorded
    // index reproduces the original vector.
    for (std::size_t i = clip.notes.size(); i-- > 0;) {
        const core::NoteId id = clip.notes[i].id;
        if (std::ranges::find(m_remove, id) == m_remove.end()) {
            continue;
        }
        m_removed.push_back(Detached{.index = i,
                                     .note = clip.notes[i],
                                     .extras = clip.setExtras(NoteExtras{
                                         .note = id, .slide = {}, .pitchCurve = {}, .lyric = {}})});
        clip.notes.erase(clip.notes.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void EditNotes::applyAdditions(Project& project, NoteClip& clip) {
    if (m_add.empty()) {
        return;
    }
    m_marks = project.idMarks();
    m_created.reserve(m_add.size());
    clip.notes.reserve(clip.notes.size() + m_add.size());
    for (const Note& input : m_add) {
        Note note = sanitised(input);
        note.id = project.newNoteId();
        m_created.push_back(note.id);
        clip.notes.push_back(note);
    }
}

void EditNotes::revert(Project& project) {
    Pattern* pattern = project.find(m_pattern);
    if (pattern == nullptr) {
        return;
    }
    if (m_removedClip || (m_createdClip && pattern->clipFor(m_channel) == nullptr)) {
        const std::size_t at = std::min(m_removedClipIndex, pattern->noteClips.size());
        pattern->noteClips.insert(pattern->noteClips.begin() + static_cast<std::ptrdiff_t>(at),
                                  NoteClip{.channel = m_channel, .notes = {}, .extras = {}});
    }
    NoteClip* clip = pattern->clipFor(m_channel);
    if (clip == nullptr) {
        return;
    }
    if (!m_created.empty()) {
        std::erase_if(clip->notes, [this](const Note& note) {
            return std::ranges::find(m_created, note.id) != m_created.end();
        });
        project.restoreIdMarks(m_marks);
    }
    for (const Detached& detached : std::views::reverse(m_removed)) {
        const auto at = static_cast<std::ptrdiff_t>(std::min(detached.index, clip->notes.size()));
        clip->notes.insert(clip->notes.begin() + at, detached.note);
        static_cast<void>(clip->setExtras(detached.extras));
    }
    for (const Note& previous : m_previous) {
        const auto it = std::ranges::find(clip->notes, previous.id, &Note::id);
        if (it != clip->notes.end()) {
            *it = previous;
        }
    }
    if (m_createdClip && clip->notes.empty()) {
        std::erase_if(pattern->noteClips,
                      [this](const NoteClip& c) { return c.channel == m_channel; });
    }
    m_previous.clear();
    m_removed.clear();
    m_created.clear();
}

std::string_view EditNotes::name() const noexcept {
    return m_label;
}

CommandId EditNotes::kind() const noexcept {
    return CommandId::kEditNotes;
}

DirtyMask EditNotes::dirty() const noexcept {
    return dirty::kPatterns;
}

} // namespace adx::project
