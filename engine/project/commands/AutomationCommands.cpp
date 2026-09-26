#include "engine/project/commands/AutomationCommands.h"

#include <algorithm>
#include <utility>

namespace adx::project {
namespace {

/// The lane list a clip lives in: a pattern's, or the playlist's.
[[nodiscard]] std::vector<AutomationClip>* lanesFor(Project& project,
                                                    core::PatternId pattern) noexcept {
    if (!pattern.valid()) {
        return &project.playlist.autoClips;
    }
    Pattern* owner = project.find(pattern);
    return owner == nullptr ? nullptr : &owner->autoClips;
}

/// Which list holds `id`, and the pattern that owns it (invalid for the playlist).
[[nodiscard]] std::vector<AutomationClip>* locate(Project& project, core::AutomationClipId id,
                                                  core::PatternId& owner) noexcept {
    for (Pattern& pattern : project.patterns) {
        if (std::ranges::find(pattern.autoClips, id, &AutomationClip::id) !=
            pattern.autoClips.end()) {
            owner = pattern.id;
            return &pattern.autoClips;
        }
    }
    if (std::ranges::find(project.playlist.autoClips, id, &AutomationClip::id) !=
        project.playlist.autoClips.end()) {
        owner = core::PatternId{};
        return &project.playlist.autoClips;
    }
    return nullptr;
}

} // namespace

// --- AddAutomationClip -------------------------------------------------------

AddAutomationClip::AddAutomationClip(core::PatternId pattern, AutomationClip prototype)
    : m_pattern(pattern), m_prototype(std::move(prototype)) {}

void AddAutomationClip::apply(Project& project) {
    std::vector<AutomationClip>* lanes = lanesFor(project, m_pattern);
    if (lanes == nullptr) {
        return;
    }
    m_marks = project.idMarks();
    m_id = project.newAutomationClipId(m_prototype.id);
    AutomationClip clip = m_prototype;
    clip.id = m_id;
    lanes->push_back(std::move(clip));
}

void AddAutomationClip::revert(Project& project) {
    if (!m_id.valid()) {
        return;
    }
    if (std::vector<AutomationClip>* lanes = lanesFor(project, m_pattern)) {
        std::erase_if(*lanes, [this](const AutomationClip& clip) { return clip.id == m_id; });
    }
    project.restoreIdMarks(m_marks);
    m_id = core::AutomationClipId{};
}

std::string_view AddAutomationClip::name() const noexcept {
    return "Add automation";
}

CommandId AddAutomationClip::kind() const noexcept {
    return CommandId::kAddAutomationClip;
}

DirtyMask AddAutomationClip::dirty() const noexcept {
    return m_pattern.valid() ? dirty::kPatterns : dirty::kPlaylist;
}

// --- RemoveAutomationClip ----------------------------------------------------

RemoveAutomationClip::RemoveAutomationClip(core::AutomationClipId id) : m_id(id) {}

void RemoveAutomationClip::apply(Project& project) {
    std::vector<AutomationClip>* lanes = locate(project, m_id, m_pattern);
    if (lanes == nullptr) {
        m_removed = false;
        return;
    }
    const auto match = std::ranges::find(*lanes, m_id, &AutomationClip::id);
    m_index = static_cast<std::size_t>(std::distance(lanes->begin(), match));
    m_previous = *match;
    lanes->erase(match);
    m_removed = true;
}

void RemoveAutomationClip::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    std::vector<AutomationClip>* lanes = lanesFor(project, m_pattern);
    if (lanes == nullptr) {
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, lanes->size()));
    lanes->insert(lanes->begin() + at, m_previous);
}

std::string_view RemoveAutomationClip::name() const noexcept {
    return "Remove automation";
}

CommandId RemoveAutomationClip::kind() const noexcept {
    return CommandId::kRemoveAutomationClip;
}

DirtyMask RemoveAutomationClip::dirty() const noexcept {
    return dirty::kPatterns | dirty::kPlaylist;
}

// --- SetBreakpoints ----------------------------------------------------------

SetBreakpoints::SetBreakpoints(core::AutomationClipId id, std::vector<Breakpoint> points)
    : m_id(id), m_points(std::move(points)) {
    // Sorted here rather than in apply(), so that apply() is a plain assignment and
    // the invariant "a lane's points are ascending" holds for every path into the
    // model, including the parser's.
    std::ranges::stable_sort(m_points, {}, &Breakpoint::at);
}

void SetBreakpoints::apply(Project& project) {
    AutomationClip* clip = project.findAutomationClip(m_id);
    if (clip == nullptr) {
        return;
    }
    m_previous = clip->points;
    clip->points = m_points;
}

void SetBreakpoints::revert(Project& project) {
    if (AutomationClip* clip = project.findAutomationClip(m_id)) {
        clip->points = m_previous;
    }
}

std::string_view SetBreakpoints::name() const noexcept {
    return "Edit automation";
}

CommandId SetBreakpoints::kind() const noexcept {
    return CommandId::kSetBreakpoints;
}

DirtyMask SetBreakpoints::dirty() const noexcept {
    return dirty::kPatterns | dirty::kPlaylist;
}

std::uint64_t SetBreakpoints::targetKey() const noexcept {
    return m_id.value;
}

bool SetBreakpoints::coalesceWith(const Command& next) {
    m_points = static_cast<const SetBreakpoints&>(next).m_points;
    return true;
}

} // namespace adx::project
