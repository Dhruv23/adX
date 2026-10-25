// The two commands a reload needs that no editor does (phase_7.md §4.2).
//
// format::applyPatch() turns a re-parsed file into the narrowest commands it can - a
// velocity is SetNoteValue, a knob is SetChannelParam - and these two cover the rest:
//
//   - SetNoteClip replaces one channel's notes in one pattern, keeping the ids the
//     patch matched (so a note that is still in the file keeps sounding) and adopting
//     the ones it allocated for new notes;
//   - ReplaceProject is the remainder: whatever structural change the narrow commands
//     do not express - a channel added, a pattern removed, a route rewired. The patch
//     has already remapped the new text into the project's ids, so even this keeps
//     every surviving entity's node, and with it its voices and its tail.
//
// Both are one undo step each, and the patch groups them, so a reload is one history
// entry however big it is.
#pragma once

#include <optional>
#include <string_view>

#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

/// Sets (or, given nullopt, removes) one channel's note clip in one pattern. The
/// clip's note ids are kept as given and the id counter adopts them.
class SetNoteClip final : public Command {
public:
    SetNoteClip(core::PatternId pattern, core::ChannelId channel, std::optional<NoteClip> clip);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::PatternId m_pattern;
    core::ChannelId m_channel;
    std::optional<NoteClip> m_clip;
    std::optional<NoteClip> m_previous;
    std::size_t m_index{0};
    IdMarks m_marks;
};

/// Replaces the whole project with `next`, id counters included. Where the sample
/// files are looked up from is not part of a project's content and is kept.
class ReplaceProject final : public Command {
public:
    explicit ReplaceProject(Project next);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    Project m_next;
    Project m_previous;
};

} // namespace adx::project
