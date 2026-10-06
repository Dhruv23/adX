// One gesture's worth of note edits, as one command (phase_5.md §4.7).
//
// Every piano-roll tool ends in exactly one command, which is what makes them all
// undoable without per-tool undo code. Most gestures are not one of the single-purpose
// commands in NoteCommands.h: a slice shortens one note and adds another, a glue
// lengthens one and deletes the rest, a quantize moves every selected note by a
// different amount. A command per piece would be several history entries for one
// gesture; a group would work but leaves the shape of the edit to whoever remembers to
// open one. So the tools describe the whole edit - which notes go, which change and to
// what, which are new - and this applies it.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/project/Note.h"
#include "engine/project/Pattern.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

class EditNotes final : public Command {
public:
    /// `update` notes are matched by id and replace the stored note wholesale (the id
    /// is kept, so a note's extras stay attached). `add` notes get fresh ids; theirs
    /// are ignored. An id in `remove` or `update` that the clip does not hold is
    /// skipped, so a stale selection cannot fail an edit.
    EditNotes(core::PatternId pattern, core::ChannelId channel, std::vector<core::NoteId> remove,
              std::vector<Note> update, std::vector<Note> add, std::string label = "Edit notes");

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    /// The ids apply() gave the added notes, in input order.
    [[nodiscard]] const std::vector<core::NoteId>& created() const noexcept {
        return m_created;
    }

private:
    struct Detached {
        std::size_t index{0};
        Note note;
        NoteExtras extras;
    };

    void applyUpdates(NoteClip& clip);
    void applyRemovals(NoteClip& clip);
    void applyAdditions(Project& project, NoteClip& clip);

    core::PatternId m_pattern;
    core::ChannelId m_channel;
    std::vector<core::NoteId> m_remove;
    std::vector<Note> m_update;
    std::vector<Note> m_add;
    std::string m_label;

    /// What apply() replaced, so revert() can put it back.
    std::vector<Note> m_previous;
    /// Descending by index, as RemoveNotes keeps them.
    std::vector<Detached> m_removed;
    std::vector<core::NoteId> m_created;
    IdMarks m_marks;
    /// apply() created the clip (nothing was there to add to) ...
    bool m_createdClip{false};
    /// ... or emptied it and took it out, from this index. An empty clip would be
    /// written as a `NOTES` header with no body, which does not load (ADX0010).
    bool m_removedClip{false};
    std::size_t m_removedClipIndex{0};
};

} // namespace adx::project
