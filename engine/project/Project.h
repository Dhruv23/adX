// The aggregate: one mutable main-thread truth for everything a `.adx` file holds.
//
// Two decisions here are load-bearing for every phase after this one.
//
// **Values, not pointers.** `std::vector<Channel>`, never
// `std::vector<std::unique_ptr<Channel>>`. Values are cheaper, copyable - which is
// what the undo system uses - and contiguous, which is what Phase 3 flattens. Ids,
// never pointers, are the stable handle, and `find()` is a linear scan over a
// vector that is realistically under 200 entries. The one place that would care
// about the scan is Phase 3, and it builds an index instead.
//
// **Main thread only.** This file uses std::vector and std::string freely and the
// audio thread must never reach into it. Phase 3 builds a flattened POD snapshot
// and hands over a pointer (phase_2.md §8).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/TempoMap.h"
#include "engine/core/Time.h"
#include "engine/format/adx/Document.h"
#include "engine/project/Automation.h"
#include "engine/project/Channel.h"
#include "engine/project/Mixer.h"
#include "engine/project/Pattern.h"
#include "engine/project/Playlist.h"
#include "engine/project/Resources.h"

namespace adx::project {

/// The format version this build writes.
inline constexpr int kAdxVersion = 2;

struct ProjectMeta {
    std::string title;
    std::string author;
    /// ISO-8601, opaque to the engine.
    std::string created;
    /// A4 in Hz.
    double tuning{440.0};
    /// ADX_VERSION as it was read. A file from a newer adX keeps its own number, so
    /// saving it does not quietly claim it is older than it is.
    int version{kAdxVersion};

    /// Presence means enabled - v1's rule, preserved deliberately, because a
    /// separate enable flag and a range that disagree is a state nobody wants.
    bool loopEnabled{false};
    core::Ticks loopStart;
    core::Ticks loopEnd;

    [[nodiscard]] friend bool operator==(const ProjectMeta&, const ProjectMeta&) noexcept = default;
};

struct Marker {
    core::MarkerId id;
    core::Ticks at;
    std::string name;

    [[nodiscard]] friend bool operator==(const Marker&, const Marker&) noexcept = default;
};

/// Every id counter's position, captured together.
///
/// A create command saves one of these before it allocates and restores it on
/// revert, so redoing the command hands out the same id it handed out the first
/// time. Without that, undo-then-redo produces a structurally identical project
/// that writes different `insert.N` references - which is what
/// `undo_redo_symmetry` would catch, on text that looks correct.
struct IdMarks {
    std::uint32_t channel{0};
    std::uint32_t pattern{0};
    std::uint32_t note{0};
    std::uint32_t insert{0};
    std::uint32_t slot{0};
    std::uint32_t send{0};
    std::uint32_t route{0};
    std::uint32_t playlistTrack{0};
    std::uint32_t item{0};
    std::uint32_t marker{0};
    std::uint32_t sample{0};
    std::uint32_t automationClip{0};

    [[nodiscard]] friend bool operator==(const IdMarks&, const IdMarks&) noexcept = default;
};

class Project {
public:
    ProjectMeta meta;
    core::TempoMap tempo;
    std::vector<Channel> channels;
    std::vector<Pattern> patterns;
    Playlist playlist;
    Mixer mixer;
    Resources resources;
    std::vector<Marker> markers;

    /// Everything the parser did not understand, carried so the writer can put it
    /// back. This is the one place the project layer depends on the format layer,
    /// and it is deliberate: residue is defined by the file, not by the model.
    format::DocumentResidue residue;

    /// Allocates the next id of each kind, or adopts `wanted` when it is valid.
    ///
    /// Adopting matters for the four entity kinds a `.adx` file names by number -
    /// inserts, slots, sends and playlist tracks. Their numbers are part of the
    /// file's reference syntax (`insert.3.slot.7.mix`), so a load that renumbered
    /// them would rewrite every automation path on the first save. Adopting instead
    /// keeps the counter above whatever the file used, so the next new entity still
    /// gets an id nothing has ever had.
    [[nodiscard]] core::ChannelId newChannelId(core::ChannelId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_channelIds.observe(wanted);
            return wanted;
        }
        return m_channelIds.next();
    }
    [[nodiscard]] core::PatternId newPatternId(core::PatternId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_patternIds.observe(wanted);
            return wanted;
        }
        return m_patternIds.next();
    }
    [[nodiscard]] core::NoteId newNoteId(core::NoteId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_noteIds.observe(wanted);
            return wanted;
        }
        return m_noteIds.next();
    }
    [[nodiscard]] core::InsertId newInsertId(core::InsertId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_insertIds.observe(wanted);
            return wanted;
        }
        return m_insertIds.next();
    }
    [[nodiscard]] core::SlotId newSlotId(core::SlotId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_slotIds.observe(wanted);
            return wanted;
        }
        return m_slotIds.next();
    }
    [[nodiscard]] core::SendId newSendId(core::SendId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_sendIds.observe(wanted);
            return wanted;
        }
        return m_sendIds.next();
    }
    [[nodiscard]] core::RouteId newRouteId(core::RouteId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_routeIds.observe(wanted);
            return wanted;
        }
        return m_routeIds.next();
    }
    [[nodiscard]] core::PlaylistTrackId
    newPlaylistTrackId(core::PlaylistTrackId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_playlistTrackIds.observe(wanted);
            return wanted;
        }
        return m_playlistTrackIds.next();
    }
    [[nodiscard]] core::ItemId newItemId(core::ItemId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_itemIds.observe(wanted);
            return wanted;
        }
        return m_itemIds.next();
    }
    [[nodiscard]] core::MarkerId newMarkerId(core::MarkerId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_markerIds.observe(wanted);
            return wanted;
        }
        return m_markerIds.next();
    }
    [[nodiscard]] core::SampleId newSampleId(core::SampleId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_sampleIds.observe(wanted);
            return wanted;
        }
        return m_sampleIds.next();
    }
    [[nodiscard]] core::AutomationClipId
    newAutomationClipId(core::AutomationClipId wanted = {}) noexcept {
        if (wanted.valid()) {
            m_automationClipIds.observe(wanted);
            return wanted;
        }
        return m_automationClipIds.next();
    }

    [[nodiscard]] IdMarks idMarks() const noexcept;
    void restoreIdMarks(const IdMarks& marks) noexcept;

    [[nodiscard]] const Channel* find(core::ChannelId id) const noexcept;
    [[nodiscard]] Channel* find(core::ChannelId id) noexcept;
    [[nodiscard]] const Pattern* find(core::PatternId id) const noexcept;
    [[nodiscard]] Pattern* find(core::PatternId id) noexcept;
    [[nodiscard]] const Marker* find(core::MarkerId id) const noexcept;
    [[nodiscard]] Marker* find(core::MarkerId id) noexcept;

    [[nodiscard]] const Channel* findChannelByName(std::string_view name) const noexcept;
    [[nodiscard]] const Pattern* findPatternByName(std::string_view name) const noexcept;

    /// Automation clips live in two places - inside patterns and directly in the
    /// playlist - and a ParamRef does not say which. This looks in both.
    [[nodiscard]] const AutomationClip*
    findAutomationClip(core::AutomationClipId id) const noexcept;
    [[nodiscard]] AutomationClip* findAutomationClip(core::AutomationClipId id) noexcept;

    /// The insert every channel falls back to. Invalid on an empty project - a new
    /// project gets its master through a command like everything else, so that
    /// creating one is undoable and produces the same ids every time.
    [[nodiscard]] const Insert* master() const noexcept {
        return mixer.find(mixer.master);
    }

    /// Total length: the end of the last playlist item, or zero for an empty
    /// arrangement. What `adx info` prints and what an export uses as its bound.
    [[nodiscard]] core::Ticks contentLength() const noexcept;

    [[nodiscard]] friend bool operator==(const Project&, const Project&) noexcept = default;

private:
    core::IdCounter<core::ChannelTag> m_channelIds;
    core::IdCounter<core::PatternTag> m_patternIds;
    core::IdCounter<core::NoteTag> m_noteIds;
    core::IdCounter<core::InsertTag> m_insertIds;
    core::IdCounter<core::SlotTag> m_slotIds;
    core::IdCounter<core::SendTag> m_sendIds;
    core::IdCounter<core::RouteTag> m_routeIds;
    core::IdCounter<core::PlaylistTrackTag> m_playlistTrackIds;
    core::IdCounter<core::ItemTag> m_itemIds;
    core::IdCounter<core::MarkerTag> m_markerIds;
    core::IdCounter<core::SampleTag> m_sampleIds;
    core::IdCounter<core::AutomationClipTag> m_automationClipIds;
};

} // namespace adx::project
