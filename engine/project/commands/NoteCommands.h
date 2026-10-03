// Commands on notes.
//
// These are the only commands that are routinely issued in bulk, and the shape
// reflects it: `AddNotes` carries a whole clip's worth rather than being called once
// per note. That is what keeps loading a 100k-note file under the half-second budget
// (phase_2.md §9) - a command per note would mean 100k heap allocations and 100k
// history entries to coalesce.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/Note.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

/// Extras for the note at `index` in an AddNotes input. The extras' own `note` field
/// is ignored: the id does not exist until apply() assigns it.
struct NoteExtrasAt {
    std::size_t index{0};
    NoteExtras extras;
};

/// Adds notes to one channel's clip in one pattern, creating the clip if it is not
/// there yet. Note ids are assigned by apply(); the ids on the input are ignored.
class AddNotes final : public Command {
public:
    AddNotes(core::PatternId pattern, core::ChannelId channel, std::vector<Note> notes,
             std::vector<NoteExtrasAt> extras = {});

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    /// The ids apply() assigned, in input order. Empty before the first apply.
    [[nodiscard]] const std::vector<core::NoteId>& created() const noexcept {
        return m_created;
    }

private:
    core::PatternId m_pattern;
    core::ChannelId m_channel;
    std::vector<Note> m_notes;
    std::vector<NoteExtrasAt> m_extras;
    std::vector<core::NoteId> m_created;
    IdMarks m_marks;
    /// True when apply() had to create the clip, so revert() knows to remove it
    /// again rather than leaving an empty one behind - an empty clip would be
    /// written out as a `NOTES <channel>` header with no body, which is ADX0010.
    bool m_createdClip{false};
};

class RemoveNotes final : public Command {
public:
    RemoveNotes(core::PatternId pattern, core::ChannelId channel, std::vector<core::NoteId> ids);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    struct Detached {
        std::size_t index{0};
        Note note;
        NoteExtras extras;
    };

    core::PatternId m_pattern;
    core::ChannelId m_channel;
    std::vector<core::NoteId> m_ids;
    /// Descending by index; revert() restores them in reverse, ascending.
    std::vector<Detached> m_removed;
};

/// Moves notes by a tick delta and a semitone delta. The canonical drag command, and
/// the reason CommandStack coalesces at all.
class MoveNotes final : public Command {
public:
    MoveNotes(core::PatternId pattern, core::ChannelId channel, std::vector<core::NoteId> ids,
              core::Ticks deltaTicks, int deltaPitch);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PatternId m_pattern;
    core::ChannelId m_channel;
    std::vector<core::NoteId> m_ids;
    core::Ticks m_deltaTicks;
    int m_deltaPitch{0};
    /// What apply() actually did. A move is clamped - a note cannot start before the
    /// pattern or leave the MIDI range - so the delta that went in is not always the
    /// delta that came out, and revert() has to undo the latter.
    std::vector<std::pair<core::Ticks, int>> m_applied;
};

/// Note fields that are a number or a flag.
enum class NoteField : std::uint8_t {
    Start,
    Length,
    Pitch,
    Velocity,
    FineTuneCents,
    ReleaseVelocity,
    Pan,
    Cutoff,
    Resonance,
};

class SetNoteValue final : public Command {
public:
    SetNoteValue(core::PatternId pattern, core::ChannelId channel, std::vector<core::NoteId> ids,
                 NoteField field, double value);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PatternId m_pattern;
    core::ChannelId m_channel;
    std::vector<core::NoteId> m_ids;
    NoteField m_field{NoteField::Velocity};
    double m_value{0.0};
    std::vector<double> m_previous;
};

/// Which part of a note's extras a SetNoteExtra command replaces.
enum class NoteExtra : std::uint8_t { Slide, PitchCurve, Lyric };

/// Replaces one part of one note's extras. The three public commands below are this
/// with the part fixed, so each is one gesture and one history entry (phase_2.md
/// §4.9) while sharing one implementation of the side table's bookkeeping.
class SetNoteExtra : public Command {
public:
    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

protected:
    SetNoteExtra(core::PatternId pattern, core::ChannelId channel, core::NoteId note,
                 NoteExtra part, NoteExtras value);

private:
    core::PatternId m_pattern;
    core::ChannelId m_channel;
    core::NoteId m_note;
    NoteExtra m_part;
    /// Only the `m_part` member is read.
    NoteExtras m_value;
    std::optional<NoteExtras> m_previous;
};

/// Sets or (with nullopt) clears a note's slide.
class SetNoteSlide final : public SetNoteExtra {
public:
    SetNoteSlide(core::PatternId pattern, core::ChannelId channel, core::NoteId note,
                 std::optional<NoteSlide> slide);
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
};

/// Replaces a note's pitch curve; an empty curve clears it.
class SetPitchCurve final : public SetNoteExtra {
public:
    SetPitchCurve(core::PatternId pattern, core::ChannelId channel, core::NoteId note,
                  std::vector<PitchPoint> curve);
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
};

/// Sets a note's lyric; an empty string clears it.
class SetLyric final : public SetNoteExtra {
public:
    SetLyric(core::PatternId pattern, core::ChannelId channel, core::NoteId note,
             std::string lyric);
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
};

} // namespace adx::project
