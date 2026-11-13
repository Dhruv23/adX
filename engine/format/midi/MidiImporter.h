// adx-thread: main
//
// Standard MIDI File import, ported from _archive/src-cpp/src/MidiImporter.cpp
// (phase_8.md §3.8): dependency-free, running status, note-on at velocity 0 as
// note-off, same-pitch overlaps paired last-in-first-out, a format-0 track split per
// channel, anything still sounding at the end of a track closed there.
//
// What the port adds: every tempo change and time signature rather than the first
// tempo only, marker meta events, ticks converted to adX's exactly (MidiFile.h), and a
// reason instead of a line on stderr when a file cannot be read.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "engine/format/midi/MidiFile.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::format::midi {

/// Parses SMF bytes. Nullopt with `error` set for a file that is not one, or that uses
/// SMPTE time division.
[[nodiscard]] std::optional<MidiSong> parseMidi(std::span<const std::uint8_t> bytes,
                                                std::string& error);
[[nodiscard]] std::optional<MidiSong> readMidiFile(const std::filesystem::path& path,
                                                   std::string& error);

/// How importSong lays the song out.
enum class ImportLayout : std::uint8_t {
    /// One channel per track, as Phase 8 imports.
    PerTrack,
    /// "Play on one GM channel" (phase_12.md §4.7.5): one 16-part `gm` channel whose
    /// notes keep their MIDI channels as parts. Each part's first program, bank,
    /// volume, expression and pan set its parameters; later changes become stepped
    /// automation lanes in the pattern; a held sustain pedal lengthens its notes.
    GmParts,
};

/// Adds `song` to `project` through commands, as one undo step: its tempo map and
/// meters, one channel per track (named after it) - or one `gm` channel for them all -
/// one pattern holding the note clips, that pattern placed at the start, and its
/// markers. Returns the pattern's name.
std::string importSong(const MidiSong& song, project::Project& project,
                       project::CommandStack& stack, const std::string& patternName = "MIDI",
                       ImportLayout layout = ImportLayout::PerTrack);

} // namespace adx::format::midi
