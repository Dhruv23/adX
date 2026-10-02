// The one way to change a project.
//
// FINAL_PLAN §4: "the `.adx` writer, the GUI, and the Python scripting API all
// mutate the project through the *same* command set - which is what guarantees they
// can never drift apart." The non-negotiable consequence, and the one that is
// easiest to quietly skip: **the parser mutates through commands too.** Loading a
// file is therefore undoable, which falls out for free, and `parser_uses_commands`
// is the test that keeps it true.
#pragma once

#include <cstdint>
#include <string_view>

namespace adx::project {

class Project;

/// What a command touched.
///
/// It exists so Phase 3 can rebuild only the affected part of the render snapshot
/// instead of the whole project on every note drag. Designing it in now costs one
/// enum; retrofitting it costs a snapshot rewrite.
using DirtyMask = std::uint32_t;

namespace dirty {
inline constexpr DirtyMask kNone = 0;
inline constexpr DirtyMask kMeta = 1U << 0U;
inline constexpr DirtyMask kTempo = 1U << 1U;
inline constexpr DirtyMask kChannels = 1U << 2U;
inline constexpr DirtyMask kPatterns = 1U << 3U;
inline constexpr DirtyMask kPlaylist = 1U << 4U;
inline constexpr DirtyMask kMixer = 1U << 5U;
inline constexpr DirtyMask kRouting = 1U << 6U;
inline constexpr DirtyMask kResources = 1U << 7U;
inline constexpr DirtyMask kMarkers = 1U << 8U;
inline constexpr DirtyMask kAll = 0x1FFU;
} // namespace dirty

/// Identifies a command's type without RTTI.
///
/// Coalescing compares these, and a history panel groups by them. An enum rather
/// than typeid because the comparison happens on every execute() and because a
/// stable numeric id is what a future serialised macro would need.
enum class CommandId : std::uint16_t {
    kGroup,

    kSetMeta,
    kSetLoop,
    kSetTempoEvent,
    kRemoveTempoEvent,
    kSetMeterEvent,
    kRemoveMeterEvent,
    kAddMarker,
    kRemoveMarker,
    kMoveMarker,

    kAddChannel,
    kRemoveChannel,
    kRenameChannel,
    kSetChannelValue,
    kSetChannelOutput,
    kSetChannelParam,
    kSetChannelArp,

    kAddPattern,
    kRemovePattern,
    kRenamePattern,
    kSetPatternLength,
    kSetMiniNotation,

    kAddNotes,
    kRemoveNotes,
    kMoveNotes,
    kSetNoteValue,

    kAddPlaylistTrack,
    kRemovePlaylistTrack,
    kRenamePlaylistTrack,
    kAddPlaylistItem,
    kRemovePlaylistItem,
    kMovePlaylistItem,

    kAddInsert,
    kRemoveInsert,
    kRenameInsert,
    kSetInsertValue,
    kAddSlot,
    kRemoveSlot,
    kSetSlotValue,
    kAddSend,
    kRemoveSend,
    kSetSendLevel,
    kAddRoute,
    kRemoveRoute,
    kSetMaster,

    kAddAutomationClip,
    kRemoveAutomationClip,
    kSetBreakpoints,

    kAddSample,
};

class Command {
public:
    Command() = default;
    virtual ~Command() = default;

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    Command(Command&&) = delete;
    Command& operator=(Command&&) = delete;

    virtual void apply(Project& project) = 0;
    virtual void revert(Project& project) = 0;

    /// For the Phase 6 history panel. A string_view over a literal, never owned.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual CommandId kind() const noexcept = 0;
    [[nodiscard]] virtual DirtyMask dirty() const noexcept = 0;

    /// Absorbs `next` into this command, returning true if it did.
    ///
    /// Not noexcept: absorbing a rename or a breakpoint list copies strings and
    /// vectors, and a declaration that promised otherwise would turn an allocation
    /// failure into std::terminate.
    ///
    /// Only ever called with a command of the same kind and the same targetKey, so
    /// an implementation may static_cast without checking - see CommandStack's
    /// coalescing rule.
    [[nodiscard]] virtual bool coalesceWith(const Command& next) {
        (void)next;
        return false;
    }

    /// Identifies what this command acts on, for coalescing. Two commands with
    /// different target keys never coalesce, so a drag on one note and a drag on
    /// another stay separate history entries.
    [[nodiscard]] virtual std::uint64_t targetKey() const noexcept {
        return 0;
    }
};

} // namespace adx::project
