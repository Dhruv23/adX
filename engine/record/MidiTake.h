// adx-thread: main
//
// MIDI takes (STATE.md P9-6): what a keyboard played while the arrangement rolled with
// MIDI recording on, turned into notes. The audio thread logs each message at the
// arrangement tick it played at (EngineCore::logTake); this side writes the log as a
// Standard MIDI File and reads it back with the importer, so a take bends exactly the
// way an imported file does - pitch bend in its RPN 0 range into Note.pitchCurve, and an
// MPE zone's member channels each bending their own note.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "engine/core/Ids.h"
#include "engine/format/midi/MidiFile.h"
#include "engine/midi/MidiEvent.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::record {

/// The take's notes, ticks relative to `origin`; anything still held at `stop` ends
/// there. Nullopt when the log has no note in it.
[[nodiscard]] std::optional<format::midi::MidiSong>
takeSong(std::span<const midi::TakeEvent> events, std::int64_t origin, std::int64_t stop);

/// Adds the take to `project` as one undo step: a pattern of `channel`'s notes starting
/// at `origin` (floored to its bar), placed on a new playlist track. Returns the
/// pattern's name, or empty when the take held no notes.
std::string commitTake(std::span<const midi::TakeEvent> events, std::int64_t origin,
                       std::int64_t stop, core::ChannelId channel, project::Project& project,
                       project::CommandStack& stack);

} // namespace adx::record
