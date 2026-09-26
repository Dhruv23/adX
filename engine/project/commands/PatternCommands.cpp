#include "engine/project/commands/PatternCommands.h"

#include <algorithm>
#include <ranges>
#include <utility>
#include <variant>

namespace adx::project {

// --- AddPattern --------------------------------------------------------------

AddPattern::AddPattern(Pattern prototype) : m_prototype(std::move(prototype)) {}

void AddPattern::apply(Project& project) {
    m_marks = project.idMarks();
    m_id = project.newPatternId(m_prototype.id);
    Pattern pattern = m_prototype;
    pattern.id = m_id;
    for (NoteClip& clip : pattern.noteClips) {
        for (Note& note : clip.notes) {
            note.id = project.newNoteId(note.id);
        }
    }
    for (AutomationClip& clip : pattern.autoClips) {
        clip.id = project.newAutomationClipId(clip.id);
    }
    project.patterns.push_back(std::move(pattern));
}

void AddPattern::revert(Project& project) {
    std::erase_if(project.patterns, [this](const Pattern& p) { return p.id == m_id; });
    project.restoreIdMarks(m_marks);
    m_id = core::PatternId{};
}

std::string_view AddPattern::name() const noexcept {
    return "Add pattern";
}

CommandId AddPattern::kind() const noexcept {
    return CommandId::kAddPattern;
}

DirtyMask AddPattern::dirty() const noexcept {
    return dirty::kPatterns;
}

// --- RemovePattern -----------------------------------------------------------

RemovePattern::RemovePattern(core::PatternId id) : m_id(id) {}

void RemovePattern::apply(Project& project) {
    const auto match = std::ranges::find(project.patterns, m_id, &Pattern::id);
    m_removed = match != project.patterns.end();
    if (!m_removed) {
        return;
    }
    m_index = static_cast<std::size_t>(std::distance(project.patterns.begin(), match));
    m_previous = *match;
    project.patterns.erase(match);

    m_items.clear();
    for (PlaylistTrack& track : project.playlist.tracks) {
        for (std::size_t i = track.items.size(); i-- > 0;) {
            const auto* ref = std::get_if<PatternRef>(&track.items[i].content);
            if (ref == nullptr || ref->pattern != m_id) {
                continue;
            }
            m_items.push_back(DetachedItem{.track = track.id, .index = i, .item = track.items[i]});
            track.items.erase(track.items.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

void RemovePattern::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, project.patterns.size()));
    project.patterns.insert(project.patterns.begin() + at, m_previous);

    for (const DetachedItem& detached : std::views::reverse(m_items)) {
        PlaylistTrack* track = project.playlist.find(detached.track);
        if (track == nullptr) {
            continue;
        }
        const auto index =
            static_cast<std::ptrdiff_t>(std::min(detached.index, track->items.size()));
        track->items.insert(track->items.begin() + index, detached.item);
    }
}

std::string_view RemovePattern::name() const noexcept {
    return "Remove pattern";
}

CommandId RemovePattern::kind() const noexcept {
    return CommandId::kRemovePattern;
}

DirtyMask RemovePattern::dirty() const noexcept {
    return dirty::kPatterns | dirty::kPlaylist;
}

// --- RenamePattern -----------------------------------------------------------

RenamePattern::RenamePattern(core::PatternId id, std::string patternName)
    : m_id(id), m_name(std::move(patternName)) {}

void RenamePattern::apply(Project& project) {
    Pattern* pattern = project.find(m_id);
    if (pattern == nullptr) {
        return;
    }
    m_previous = pattern->name;
    pattern->name = m_name;
}

void RenamePattern::revert(Project& project) {
    Pattern* pattern = project.find(m_id);
    if (pattern != nullptr) {
        pattern->name = m_previous;
    }
}

std::string_view RenamePattern::name() const noexcept {
    return "Rename pattern";
}

CommandId RenamePattern::kind() const noexcept {
    return CommandId::kRenamePattern;
}

DirtyMask RenamePattern::dirty() const noexcept {
    return dirty::kPatterns;
}

std::uint64_t RenamePattern::targetKey() const noexcept {
    return m_id.value;
}

bool RenamePattern::coalesceWith(const Command& next) {
    m_name = static_cast<const RenamePattern&>(next).m_name;
    return true;
}

// --- SetPatternLength --------------------------------------------------------

SetPatternLength::SetPatternLength(core::PatternId id, core::Ticks length)
    : m_id(id), m_length(length) {}

void SetPatternLength::apply(Project& project) {
    Pattern* pattern = project.find(m_id);
    if (pattern == nullptr) {
        return;
    }
    m_previous = pattern->length;
    pattern->length = m_length;
}

void SetPatternLength::revert(Project& project) {
    Pattern* pattern = project.find(m_id);
    if (pattern != nullptr) {
        pattern->length = m_previous;
    }
}

std::string_view SetPatternLength::name() const noexcept {
    return "Resize pattern";
}

CommandId SetPatternLength::kind() const noexcept {
    return CommandId::kSetPatternLength;
}

DirtyMask SetPatternLength::dirty() const noexcept {
    return dirty::kPatterns;
}

std::uint64_t SetPatternLength::targetKey() const noexcept {
    return m_id.value;
}

bool SetPatternLength::coalesceWith(const Command& next) {
    m_length = static_cast<const SetPatternLength&>(next).m_length;
    return true;
}

// --- SetPatternColor ---------------------------------------------------------

SetPatternColor::SetPatternColor(core::PatternId id, Color color) : m_id(id), m_color(color) {}

void SetPatternColor::apply(Project& project) {
    Pattern* pattern = project.find(m_id);
    if (pattern == nullptr) {
        return;
    }
    m_previous = pattern->color;
    pattern->color = m_color;
}

void SetPatternColor::revert(Project& project) {
    Pattern* pattern = project.find(m_id);
    if (pattern != nullptr) {
        pattern->color = m_previous;
    }
}

std::string_view SetPatternColor::name() const noexcept {
    return "Set pattern colour";
}

CommandId SetPatternColor::kind() const noexcept {
    return CommandId::kSetPatternLength;
}

DirtyMask SetPatternColor::dirty() const noexcept {
    return dirty::kPatterns;
}

std::uint64_t SetPatternColor::targetKey() const noexcept {
    return (static_cast<std::uint64_t>(0xC0) << 32U) | m_id.value;
}

bool SetPatternColor::coalesceWith(const Command& next) {
    m_color = static_cast<const SetPatternColor&>(next).m_color;
    return true;
}

// --- SetMiniNotation ---------------------------------------------------------

SetMiniNotation::SetMiniNotation(core::PatternId pattern, core::ChannelId channel,
                                 std::vector<std::string> lines)
    : m_pattern(pattern), m_channel(channel), m_lines(std::move(lines)) {}

void SetMiniNotation::apply(Project& project) {
    Pattern* pattern = project.find(m_pattern);
    if (pattern == nullptr) {
        return;
    }
    const auto match = std::ranges::find(pattern->mini, m_channel, &MiniNotationSource::channel);
    m_existed = match != pattern->mini.end();
    if (m_existed) {
        m_index = static_cast<std::size_t>(std::distance(pattern->mini.begin(), match));
        m_previous = match->lines;
        if (m_lines.empty()) {
            pattern->mini.erase(match);
        } else {
            match->lines = m_lines;
        }
        return;
    }
    if (m_lines.empty()) {
        return;
    }
    pattern->mini.push_back(MiniNotationSource{.channel = m_channel, .lines = m_lines});
}

void SetMiniNotation::revert(Project& project) {
    Pattern* pattern = project.find(m_pattern);
    if (pattern == nullptr) {
        return;
    }
    if (!m_existed) {
        std::erase_if(pattern->mini, [this](const MiniNotationSource& source) {
            return source.channel == m_channel;
        });
        return;
    }
    const auto match = std::ranges::find(pattern->mini, m_channel, &MiniNotationSource::channel);
    if (match != pattern->mini.end()) {
        match->lines = m_previous;
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, pattern->mini.size()));
    pattern->mini.insert(pattern->mini.begin() + at,
                         MiniNotationSource{.channel = m_channel, .lines = m_previous});
}

std::string_view SetMiniNotation::name() const noexcept {
    return "Edit mini-notation";
}

CommandId SetMiniNotation::kind() const noexcept {
    return CommandId::kSetMiniNotation;
}

DirtyMask SetMiniNotation::dirty() const noexcept {
    return dirty::kPatterns;
}

std::uint64_t SetMiniNotation::targetKey() const noexcept {
    return (static_cast<std::uint64_t>(m_pattern.value) << 32U) | m_channel.value;
}

bool SetMiniNotation::coalesceWith(const Command& next) {
    // Typing in the text panel: one history entry per burst of typing, not per
    // keystroke.
    m_lines = static_cast<const SetMiniNotation&>(next).m_lines;
    return true;
}

} // namespace adx::project
