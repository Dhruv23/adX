// adx-thread: main
//
// Standard MIDI File export, type 1 (phase_8.md §3.8) - new; the archive only read.
//
//   track 0      the conductor: tempo changes, time signatures, markers
//   tracks 1..n  one per channel, named after it, its notes as the arrangement plays
//                them - every placement of every pattern, trimmed and slipped the way
//                the snapshot builder flattens them, mini-notation realised
//
// A tempo ramp has no SMF spelling; it is written as a tempo event per beat along the
// ramp. MIDI channels are assigned in channel order, skipping channel 10 (General MIDI
// percussion), wrapping after fifteen. Automation does not become CC: nothing in the
// model maps a lane to a controller number until Phase 9's MIDI learn (phase_8.md §11).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/format/midi/MidiFile.h"

namespace adx::project {
class Project;
}

namespace adx::format::midi {

/// The arrangement as a song: what writeMidi() serialises.
[[nodiscard]] MidiSong songOf(const project::Project& project);

/// SMF type 1 bytes at kExportDivision. Tracks with no notes are still written, so a
/// channel's track number is stable whatever it plays.
[[nodiscard]] std::vector<std::uint8_t> writeMidi(const MidiSong& song);

[[nodiscard]] bool exportMidiFile(const project::Project& project,
                                  const std::filesystem::path& path, std::string& error);

} // namespace adx::format::midi
