#include "engine/project/commands/PatchCommands.h"

#include <iterator>
#include <utility>

namespace adx::project {

// --- SetNoteClip -------------------------------------------------------------

SetNoteClip::SetNoteClip(core::PatternId pattern, core::ChannelId channel,
                         std::optional<NoteClip> clip)
    : m_pattern(pattern), m_channel(channel), m_clip(std::move(clip)) {}

void SetNoteClip::apply(Project& project) {
    Pattern* pattern = project.find(m_pattern);
    if (pattern == nullptr) {
        return;
    }
    m_marks = project.idMarks();
    m_previous.reset();
    const auto found = std::ranges::find(pattern->noteClips, m_channel, &NoteClip::channel);
    m_index = static_cast<std::size_t>(std::distance(pattern->noteClips.begin(), found));
    if (found != pattern->noteClips.end()) {
        m_previous = *found;
        pattern->noteClips.erase(found);
    }
    if (m_clip.has_value()) {
        NoteClip clip = *m_clip;
        clip.channel = m_channel;
        for (const Note& note : clip.notes) {
            static_cast<void>(project.newNoteId(note.id));
        }
        const std::size_t at = std::min(m_index, pattern->noteClips.size());
        pattern->noteClips.insert(pattern->noteClips.begin() + static_cast<std::ptrdiff_t>(at),
                                  std::move(clip));
    }
}

void SetNoteClip::revert(Project& project) {
    Pattern* pattern = project.find(m_pattern);
    if (pattern == nullptr) {
        return;
    }
    std::erase_if(pattern->noteClips,
                  [&](const NoteClip& clip) { return clip.channel == m_channel; });
    if (m_previous.has_value()) {
        const std::size_t at = std::min(m_index, pattern->noteClips.size());
        pattern->noteClips.insert(pattern->noteClips.begin() + static_cast<std::ptrdiff_t>(at),
                                  *m_previous);
    }
    project.restoreIdMarks(m_marks);
}

std::string_view SetNoteClip::name() const noexcept {
    return "Edit notes (text)";
}

CommandId SetNoteClip::kind() const noexcept {
    return CommandId::kSetNoteClip;
}

DirtyMask SetNoteClip::dirty() const noexcept {
    return dirty::kPatterns;
}

// --- ReplaceProject ----------------------------------------------------------

ReplaceProject::ReplaceProject(Project next) : m_next(std::move(next)) {}

void ReplaceProject::apply(Project& project) {
    m_previous = project;
    std::string base = project.resources.baseDirectory;
    project = m_next;
    project.resources.baseDirectory = std::move(base);
}

void ReplaceProject::revert(Project& project) {
    std::string base = project.resources.baseDirectory;
    project = m_previous;
    project.resources.baseDirectory = std::move(base);
}

std::string_view ReplaceProject::name() const noexcept {
    return "Reload from text";
}

CommandId ReplaceProject::kind() const noexcept {
    return CommandId::kReplaceProject;
}

DirtyMask ReplaceProject::dirty() const noexcept {
    return dirty::kAll;
}

} // namespace adx::project
