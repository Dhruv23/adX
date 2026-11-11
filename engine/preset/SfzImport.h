// adx-thread: main
//
// An SFZ import as a sampler preset (phase_12.md §4.7.2).
//
// The parser (engine/format/sfz) gives zones; this gives the `.adxpreset` around them.
// The sampler's envelope is the instrument's, not the zone's, so the preset takes the
// envelope most regions ask for and says in the import's notes how many differ.
#pragma once

#include <string>

#include "engine/format/sfz/SfzParser.h"
#include "engine/preset/Preset.h"

namespace adx::preset {

struct SfzPresetOptions {
    std::string name;
    std::string pack;
    /// Put before every sample path: `lib://vsco2ce/` for a library file, empty for
    /// paths relative to the preset.
    std::string pathPrefix;
};

/// A `sampler` preset of the import's regions. Adds to `import.notes` what the preset
/// could not carry.
[[nodiscard]] Preset presetFromSfz(format::sfz::Import& import, const SfzPresetOptions& options);

} // namespace adx::preset
