// A pattern: a named, reusable bundle of musical data.
//
// The separation that earns its keep: one instrument can appear in fifty patterns,
// and a pattern placed eight times in the playlist is one piece of data that all
// eight placements share. Iteration one bound one note list to one instrument and
// made the arrangement out of note lists, so reusing a riff meant copying it and
// editing it afterwards meant editing every copy (FINAL_PLAN §4).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/Automation.h"
#include "engine/project/Color.h"
#include "engine/project/Note.h"

namespace adx::project {

/// A glide from a note's own pitch to `targetCents` above it (negative: below),
/// starting `start` after the note does and taking `length`. Stored as cents rather
/// than as a link to the next note, so moving or deleting that note never silently
/// retargets the slide (phase_4.md §4.0).
struct NoteSlide {
    std::int16_t targetCents{0};
    /// Note-relative.
    core::Ticks start;
    core::Ticks length;
    core::Curve curve;

    [[nodiscard]] friend bool operator==(const NoteSlide&, const NoteSlide&) noexcept = default;
};

/// One point of a freeform pitch curve: `cents` from the note's pitch at `at` ticks
/// after its start. The curve is the shape of the segment that starts here.
struct PitchPoint {
    core::Ticks at;
    std::int16_t cents{0};
    core::Curve curve;

    [[nodiscard]] friend bool operator==(const PitchPoint&, const PitchPoint&) noexcept = default;
};

/// What a note may carry beyond the fixed fields: a slide, a pitch curve, a lyric.
///
/// A side table on the clip rather than members of Note, because these three are
/// variable-length and rare. Note stays trivially copyable - which undo, the snapshot
/// cache and Phase 5's zero-copy numpy view all rely on - and a note with none of them
/// costs nothing (phase_4.md §11, a correction to §4.0's `Note` sketch).
struct NoteExtras {
    core::NoteId note;
    std::optional<NoteSlide> slide;
    std::vector<PitchPoint> pitchCurve;
    /// Voice instrument (§4.13); empty means none.
    std::string lyric;

    [[nodiscard]] bool empty() const noexcept {
        return !slide.has_value() && pitchCurve.empty() && lyric.empty();
    }

    [[nodiscard]] friend bool operator==(const NoteExtras&, const NoteExtras&) noexcept = default;
};

/// The notes one channel plays in one pattern. At most one per channel per pattern.
struct NoteClip {
    core::ChannelId channel;
    std::vector<Note> notes;
    /// Sorted by note id, at most one entry per note, never an empty entry.
    std::vector<NoteExtras> extras;

    [[nodiscard]] const NoteExtras* extrasFor(core::NoteId note) const noexcept;

    /// Replaces (or, given an empty value, removes) the entry for `value.note`, keeping
    /// the table sorted. Returns what was there before, empty when nothing was.
    NoteExtras setExtras(NoteExtras value);

    [[nodiscard]] friend bool operator==(const NoteClip&, const NoteClip&) noexcept = default;
};

/// Mini-notation source text, kept verbatim.
///
/// Phase 2 stores it and does not compile it - the compiler is Phase 4's and the
/// live wiring is Phase 7's. Stored as lines rather than one blob so the writer can
/// re-indent it without having to re-split it, and so a diagnostic can name a line.
struct MiniNotationSource {
    core::ChannelId channel;
    std::vector<std::string> lines;

    [[nodiscard]] friend bool operator==(const MiniNotationSource&,
                                         const MiniNotationSource&) noexcept = default;
};

/// The default pattern length, in bars of the meter at tick 0: four bars.
inline constexpr core::Ticks kDefaultPatternLength{core::kPpq * 4 * 4};

struct Pattern {
    core::PatternId id;
    /// Unique within the project: the playlist addresses patterns by name.
    std::string name;
    Color color;

    /// Pattern-local, and may be any length - patterns are not required to be a
    /// whole number of bars, which is what makes polymetric arrangements possible.
    core::Ticks length{kDefaultPatternLength};

    std::vector<NoteClip> noteClips;
    std::vector<AutomationClip> autoClips;
    std::vector<MiniNotationSource> mini;

    [[nodiscard]] const NoteClip* clipFor(core::ChannelId channel) const noexcept;
    [[nodiscard]] NoteClip* clipFor(core::ChannelId channel) noexcept;

    [[nodiscard]] friend bool operator==(const Pattern&, const Pattern&) noexcept = default;
};

} // namespace adx::project
