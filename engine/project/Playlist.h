// The arrangement.
//
// A playlist track holds *placements*, not data. Moving a pattern in the
// arrangement never touches its notes, and editing its notes updates every
// placement - which is FL Studio's central ergonomic advantage and is impossible
// without the Pattern/PlaylistItem split (FINAL_PLAN §4).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/Automation.h"
#include "engine/project/Color.h"

namespace adx::project {

struct PatternRef {
    core::PatternId pattern;

    [[nodiscard]] friend bool operator==(const PatternRef&, const PatternRef&) noexcept = default;
};

struct AudioClipRef {
    core::SampleId sample;
    float stretch{1.0F};
    float pitchSemitones{0.0F};
    bool reverse{false};

    [[nodiscard]] friend bool operator==(const AudioClipRef&,
                                         const AudioClipRef&) noexcept = default;
};

/// A placed automation lane. The lane itself lives in Playlist::autoClips, because
/// a playlist automation clip is not owned by any pattern.
struct AutomationRef {
    core::AutomationClipId clip;

    [[nodiscard]] friend bool operator==(const AutomationRef&,
                                         const AutomationRef&) noexcept = default;
};

/// What a clip envelope drives when it does not name a project parameter: a property
/// of the placement itself.
enum class ClipTarget : std::uint8_t {
    /// The envelope targets `ClipEnvelope::target`, a project parameter.
    Param,
    Gain,
    Pan,
    PitchCents,
};

/// Automation that belongs to one placement (phase_4.md §4.0). Breakpoint times are
/// item-relative - 0 is `item.start` - and points past the item's length are not
/// played. A Param envelope applies only while the item plays; the parameter returns to
/// its base value afterwards.
struct ClipEnvelope {
    ClipTarget local{ClipTarget::Param};
    /// As written, for a Param target - kept for the same reason AutomationClip keeps
    /// it: an unresolvable target is preserved, not dropped.
    std::string targetPath;
    ParamRef target;
    std::vector<Breakpoint> points;

    [[nodiscard]] friend bool operator==(const ClipEnvelope&,
                                         const ClipEnvelope&) noexcept = default;
};

[[nodiscard]] const char* toString(ClipTarget target) noexcept;
/// `gain`, `pan`, `pitch`; anything else is a parameter path.
[[nodiscard]] bool clipTargetFromString(std::string_view name, ClipTarget& out) noexcept;

struct PlaylistItem {
    core::ItemId id;
    core::Ticks start;
    /// Zero means "the content's natural length", which for a pattern is its own
    /// length. Stored rather than always resolved, because trimming a placement
    /// shorter than its pattern is an edit the model has to be able to hold.
    core::Ticks length;
    /// Slip editing: how far into the source the placement starts. Phase 8 makes it
    /// audible; the model carries it from here so it does not have to be retrofitted
    /// into the file format later.
    core::Ticks sourceOffset;

    std::variant<PatternRef, AudioClipRef, AutomationRef> content;
    bool muted{false};
    std::vector<ClipEnvelope> envelopes;

    [[nodiscard]] friend bool operator==(const PlaylistItem&,
                                         const PlaylistItem&) noexcept = default;
};

/// The height a playlist track is drawn at, in the UI's abstract row units. In the
/// model because it is part of the project a person saved, not part of the app's
/// window state.
inline constexpr std::uint16_t kDefaultTrackHeight = 1;

struct PlaylistTrack {
    core::PlaylistTrackId id;
    std::string name;
    Color color;
    std::uint16_t height{kDefaultTrackHeight};
    bool muted{false};
    std::vector<PlaylistItem> items;

    [[nodiscard]] friend bool operator==(const PlaylistTrack&,
                                         const PlaylistTrack&) noexcept = default;
};

struct Playlist {
    std::vector<PlaylistTrack> tracks;
    /// Automation lanes placed directly in the arrangement rather than inside a
    /// pattern. Referenced by AutomationRef, addressed by id.
    std::vector<AutomationClip> autoClips;

    [[nodiscard]] const PlaylistTrack* find(core::PlaylistTrackId id) const noexcept;
    [[nodiscard]] PlaylistTrack* find(core::PlaylistTrackId id) noexcept;

    [[nodiscard]] friend bool operator==(const Playlist&, const Playlist&) noexcept = default;
};

} // namespace adx::project
