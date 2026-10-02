// A pattern: a named, reusable bundle of musical data.
//
// The separation that earns its keep: one instrument can appear in fifty patterns,
// and a pattern placed eight times in the playlist is one piece of data that all
// eight placements share. Iteration one bound one note list to one instrument and
// made the arrangement out of note lists, so reusing a riff meant copying it and
// editing it afterwards meant editing every copy (FINAL_PLAN §4).
#pragma once

#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/Automation.h"
#include "engine/project/Color.h"
#include "engine/project/Note.h"

namespace adx::project {

/// The notes one channel plays in one pattern. At most one per channel per pattern.
struct NoteClip {
    core::ChannelId channel;
    std::vector<Note> notes;

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
