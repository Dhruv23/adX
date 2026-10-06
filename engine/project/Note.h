// One note.
//
// A value type, trivially copyable, held contiguously in a NoteClip's vector. That
// is deliberate: undo copies clips wholesale, Phase 3 flattens them into a
// snapshot, and Phase 5 hands them to numpy as a zero-copy view - all three want
// notes packed, not scattered behind pointers.
#pragma once

#include <cstdint>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"

namespace adx::project {

/// Defaults for the per-note modulation lanes.
///
/// Each lane is an *offset* from the channel's own value, so zero means "leave the
/// channel alone" and a note with no lanes written in the file is a note with every
/// lane at zero. That is what lets the writer omit them, which is what keeps
/// adding a note to a one-line diff.
inline constexpr std::uint8_t kDefaultVelocity = 100;
inline constexpr std::uint16_t kDefaultReleaseVelocity = 64;

struct Note {
    core::NoteId id;
    core::Ticks start;
    core::Ticks length;

    /// MIDI note number. 60 is middle C, matching the note-name table in NoteName.h.
    std::uint8_t pitch{60};
    /// 0..127. Not a normalised float: 127 distinct values do not map onto round
    /// decimals, so a float spelling is lossy and makes `adx fmt` non-idempotent.
    std::uint8_t velocity{kDefaultVelocity};

    /// Detune, in cents, relative to the note's equal-tempered pitch.
    std::int16_t fineTuneCents{0};
    std::uint16_t releaseVelocity{kDefaultReleaseVelocity};
    /// A muted note stays in the clip and in the file but does not sound - the
    /// piano roll's Mute tool (phase_5.md §4.7), which silences without deleting.
    bool muted{false};

    /// The per-note lanes FINAL_PLAN §5.1 asks for, each an offset from the
    /// channel's value: pan in -1..1, cutoff in octaves, resonance in 0..1.
    float pan{0.0F};
    float cutoff{0.0F};
    float resonance{0.0F};

    [[nodiscard]] core::Ticks end() const noexcept {
        return start + length;
    }

    [[nodiscard]] friend bool operator==(const Note&, const Note&) noexcept = default;
};

/// Sort key for the canonical writer: notes are emitted in (start, pitch) order, so
/// two projects that differ only in edit history write the same bytes.
[[nodiscard]] inline bool noteOrderBefore(const Note& lhs, const Note& rhs) noexcept {
    if (lhs.start != rhs.start) {
        return lhs.start < rhs.start;
    }
    if (lhs.pitch != rhs.pitch) {
        return lhs.pitch < rhs.pitch;
    }
    // Two notes at the same pitch and position are legal (stacked layers), so the id
    // breaks the tie. Without it the sort is unstable across loads and the writer's
    // output depends on vector order, which undo churns.
    return lhs.id.value < rhs.id.value;
}

} // namespace adx::project
