// Phase 6's commands: the edits the playlist, the mixer's slot list and the controller
// panel make that Phases 2-4 had no command for (phase_6.md §3 rule 3: "if a needed
// command does not exist, it is added ... with a test - not worked around").
//
//   MoveSlot            reorder an insert's effects; ids, and so automation, stay put
//   SetPlaylistItem     every field of one placement at once - slip, stretch, trim,
//                       mute, move to another track - so one gesture is one command
//   SetPlaylistTrack    a track's own fields (name, colour, height, mute, solo, group)
//   MovePlaylistTrack   reorder the tracks
//   Add/Remove/Switch/RenameArrangement
//                       named playlist states, switched by swapping (§4.1)
//   Add/Remove/SetController
//                       LFO, envelope and peak controllers (§4.5)
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

/// Moves a slot to position `index` of its insert's slot list (clamped). Slots process
/// in list order (docs/adx-format-v2.md §7.5), and a slot keeps its id, so its DSP
/// state, its automation and every path naming it survive the move.
class MoveSlot final : public Command {
public:
    MoveSlot(core::SlotId slot, std::size_t index);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::SlotId m_slot;
    std::size_t m_index;
    core::InsertId m_insert;
    std::size_t m_previousIndex{0};
    bool m_moved{false};
};

/// Replaces one placement's fields with `value`'s - start, length, source offset,
/// content, mute, envelopes - keeping its id, and moves it to `destination` when that is
/// a different track. Consecutive edits of the same item coalesce, so a drag is one
/// history entry.
class SetPlaylistItem final : public Command {
public:
    SetPlaylistItem(core::PlaylistTrackId track, core::ItemId item, PlaylistItem value,
                    core::PlaylistTrackId destination);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PlaylistTrackId m_track;
    core::ItemId m_item;
    PlaylistItem m_value;
    core::PlaylistTrackId m_destination;

    std::optional<PlaylistItem> m_previous;
    std::size_t m_previousIndex{0};
    core::PlaylistTrackId m_landedOn;
};

/// Sets a track's own fields from `value`: everything but its id and items.
class SetPlaylistTrack final : public Command {
public:
    SetPlaylistTrack(core::PlaylistTrackId track, PlaylistTrack value);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PlaylistTrackId m_track;
    PlaylistTrack m_value;
    std::optional<PlaylistTrack> m_previous;
};

/// Moves a track to position `index` (clamped).
class MovePlaylistTrack final : public Command {
public:
    MovePlaylistTrack(core::PlaylistTrackId track, std::size_t index);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::PlaylistTrackId m_track;
    std::size_t m_index;
    std::size_t m_previousIndex{0};
    bool m_moved{false};
};

/// Stores a new arrangement named `name`: a copy of the live tracks, or empty.
class AddArrangement final : public Command {
public:
    AddArrangement(std::string arrangementName, bool copyLive);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    /// Where it went in Playlist::arrangements.
    [[nodiscard]] std::size_t index() const noexcept {
        return m_index;
    }

private:
    std::string m_name;
    bool m_copy;
    std::size_t m_index{0};
};

class RemoveArrangement final : public Command {
public:
    explicit RemoveArrangement(std::size_t index);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    std::size_t m_index;
    std::optional<Arrangement> m_removed;
};

/// Swaps the live tracks and name with stored arrangement `index`. Its own inverse.
class SwitchArrangement final : public Command {
public:
    explicit SwitchArrangement(std::size_t index);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    void swap(Project& project) const;
    std::size_t m_index;
};

/// Renames stored arrangement `index`, or the live one when `index` is empty.
class RenameArrangement final : public Command {
public:
    RenameArrangement(std::optional<std::size_t> index, std::string arrangementName);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    [[nodiscard]] std::string* target(Project& project) const noexcept;
    std::optional<std::size_t> m_index;
    std::string m_name;
    std::string m_previous;
};

class AddController final : public Command {
public:
    explicit AddController(Controller prototype);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::ControllerId created() const noexcept {
        return m_id;
    }

private:
    Controller m_prototype;
    IdMarks m_marks;
    core::ControllerId m_id;
};

class RemoveController final : public Command {
public:
    explicit RemoveController(core::ControllerId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::ControllerId m_id;
    std::size_t m_index{0};
    std::optional<Controller> m_removed;
};

/// Replaces a controller's settings, keeping its id. Consecutive edits of one
/// controller coalesce: a knob drag is one history entry.
class SetController final : public Command {
public:
    SetController(core::ControllerId id, Controller value);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::ControllerId m_id;
    Controller m_value;
    std::optional<Controller> m_previous;
};

/// Replaces a channel's whole instrument - type, parameters, zones, voicebank - with
/// `spec`: what applying a preset is, as one command (P4-2).
class SetInstrument final : public Command {
public:
    SetInstrument(core::ChannelId channel, InstrumentSpec spec);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::ChannelId m_channel;
    InstrumentSpec m_spec;
    std::optional<InstrumentSpec> m_previous;
};

/// Replaces a slot's effect - type, parameters and mix - keeping its id, so its place in
/// the chain and anything automating its mix survive. An effect preset dropped on a slot.
class SetSlotEffect final : public Command {
public:
    SetSlotEffect(core::SlotId slot, std::string type, std::vector<SlotParam> params, float mix);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::SlotId m_slot;
    std::string m_type;
    std::vector<SlotParam> m_params;
    float m_mix;
    std::optional<Slot> m_previous;
};

/// Sets the file a Convolution slot convolves with (a pool entry), or none for its
/// synthetic room. Structure: a different file is a different node.
class SetSlotImpulse final : public Command {
public:
    SetSlotImpulse(core::SlotId slot, core::SampleId impulse);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::SlotId m_slot;
    core::SampleId m_impulse;
    core::SampleId m_previous;
};

} // namespace adx::project
