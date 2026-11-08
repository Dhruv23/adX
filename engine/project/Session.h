// adx-thread: main
//
// The session (phase_11.md §4.1): scenes down, tracks across, and in each cell a
// launchable clip. Part of Project and of the text: `[SESSION]`, so a set is written,
// diffed and scripted like everything else.
//
// One session track is one channel (the clip's notes play through it) and is linked to
// one playlist track (where performance recording writes). Cells hold a pattern for
// now; audio and mini-notation launchables are open work (STATE.md, Phase 11).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/session/FollowAction.h"
#include "engine/session/LaunchQuantize.h"

namespace adx::project {

struct ClipSlot {
    core::PatternId pattern;
    /// The loop length; zero means the pattern's own length.
    core::Ticks length;
    bool loop{true};
    session::LaunchQuantize quantize{session::LaunchQuantize::Global};
    session::FollowAction follow{session::FollowAction::None};
    /// Passes before the follow action (and RepeatN's count).
    std::uint8_t followRepeats{1};
    /// A relaunch while playing carries on in phase instead of restarting.
    bool legato{false};
    float gain{1.0F};

    [[nodiscard]] friend bool operator==(const ClipSlot&, const ClipSlot&) noexcept = default;
};

struct SessionTrack {
    core::ChannelId channel;
    /// Where performance recording writes; invalid until one is made.
    core::PlaylistTrackId linked;
    /// One per scene row; empty where a cell has no clip.
    std::vector<std::optional<ClipSlot>> slots;
    /// Firing a scene stops this track's clip when the scene's cell here is empty.
    bool stopOnEmptyScene{true};

    [[nodiscard]] friend bool operator==(const SessionTrack&,
                                         const SessionTrack&) noexcept = default;
};

struct SessionScene {
    std::string name;
    /// Firing the scene sets this tempo at its boundary; none keeps the tempo.
    std::optional<double> tempo;

    [[nodiscard]] friend bool operator==(const SessionScene&,
                                         const SessionScene&) noexcept = default;
};

struct SessionModel {
    std::vector<SessionScene> scenes;
    std::vector<SessionTrack> tracks;
    session::LaunchQuantize quantize{session::LaunchQuantize::Bar};

    [[nodiscard]] bool empty() const noexcept {
        return scenes.empty() && tracks.empty();
    }
    /// The cell at (track, row), or null.
    [[nodiscard]] const ClipSlot* slot(std::size_t track, std::size_t row) const noexcept {
        if (track >= tracks.size() || row >= tracks[track].slots.size() ||
            !tracks[track].slots[row]) {
            return nullptr;
        }
        return &*tracks[track].slots[row];
    }

    [[nodiscard]] friend bool operator==(const SessionModel&,
                                         const SessionModel&) noexcept = default;
};

} // namespace adx::project
