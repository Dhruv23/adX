// adx-thread: main
//
// SFZ import (phase_12.md §4.7.2): the opcode subset the chosen libraries use, read
// into sampler zones.
//
// VSCO 2 CE, VCSL and Salamander ship SFZ mappings of thousands of regions; writing
// those zone lines by hand is the error-prone work this replaces. The parser reads
// <control>, <global>, <master>, <group> and <region> with their inheritance,
// `#define` and `#include`, and maps what the sampler can play: key and velocity
// ranges, the root key, tuning, volume and pan, offsets and loops, round robins
// (seq_*, and lorand/hirand as a round robin), keyswitches, release triggers, dynamic
// crossfades and choke groups.
//
// Nothing is dropped silently. Every opcode that is not applied is in the import's
// notes with its line and column, as one of three kinds: unknown (this build has never
// heard of it), ignored (understood, and it changes no sound here: a label), or
// approximated (understood, and what the sampler does differs: a per-region envelope,
// when the sampler's envelope is the instrument's).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/project/SampleZone.h"

namespace adx::format::sfz {

struct Region {
    project::SampleZone zone;
    /// default_path + sample, forward slashes, as the file spells it: relative to the
    /// .sfz file.
    std::string sample;
    /// ampeg_*, in seconds and 0..1; negative when the region does not set one.
    float attack{-1.0F};
    float decay{-1.0F};
    float sustain{-1.0F};
    float release{-1.0F};
    /// amp_veltrack as 0..1; negative when unset.
    float velocityTrack{-1.0F};
    bool oneShot{false};
    std::uint32_t line{0};
};

enum class NoteKind : std::uint8_t { Unknown, Ignored, Approximated };

struct Note {
    NoteKind kind{NoteKind::Unknown};
    std::string opcode;
    std::string detail;
    std::uint32_t line{0};
    std::uint32_t column{0};
};

struct Import {
    std::vector<Region> regions;
    std::vector<Note> notes;

    [[nodiscard]] std::size_t count(NoteKind kind) const noexcept;
};

/// Gives the text of an `#include`d file, named as the SFZ names it; false when it
/// cannot be read.
using IncludeReader = std::function<bool(std::string_view name, std::string& text)>;

/// Parses SFZ text. Never throws; what cannot be understood is a note.
[[nodiscard]] Import parseSfz(std::string_view text, const IncludeReader& include = {});

/// Reads `path` and its includes (relative to the file). False when the file itself
/// cannot be read.
[[nodiscard]] bool loadSfz(const std::filesystem::path& path, Import& out);

/// The import's notes as text, one per line: `line:column kind opcode - detail`.
[[nodiscard]] std::string reportOf(const Import& import);

} // namespace adx::format::sfz
