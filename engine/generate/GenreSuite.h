// adx-thread: main
//
// The genre suites (phase_10.md §4.6, FINAL_PLAN §5.9): docs/C418.md and docs/STAKILLAZ.md
// as executable packs. Phase 4 shipped their presets; a suite adds
//
//   a template    the channel set (each on a pack preset), the mixer (inserts, effect
//                 presets on slots, sends, the sidechain) and a playlist skeleton with
//                 section markers, and no notes
//   generators    seeded patterns idiomatic to the style, on RiffGenerator
//   a track       the template filled in: "make me a track in this style"
//
// A suite builds a project::Project, and the canonical writer turns it into text, so a
// generated track is reproducible to the byte from its seed (genre_suite_deterministic)
// and can be a golden fixture (genre_suite_renders). The app applies one as one undo
// step. Presets come from a PresetLibrary - the shipped packs - by name; a missing one
// leaves its channel on the type's defaults and says so in `notes`.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/analysis/KeyDetector.h"
#include "engine/preset/PresetLibrary.h"
#include "engine/project/Project.h"

namespace adx::generate {

struct StyleOptions {
    std::uint32_t seed{1};
    /// Bars of the whole track; the suite rounds to its section length.
    std::uint32_t bars{32};
    /// The style's own when unset.
    std::optional<double> bpm;
    std::optional<analysis::Key> key;
};

struct GeneratedTrack {
    project::Project project;
    double bpm{120.0};
    analysis::Key key;
    /// "intro 0-4, theme 4-12, ..." and any preset that was not found.
    std::vector<std::string> notes;
};

struct StyleInfo {
    std::string id;
    std::string name;
    std::string description;
    std::string pack;
};

/// Every suite, in menu order.
[[nodiscard]] const std::vector<StyleInfo>& styles();

/// The channels, mixer and playlist skeleton of `style`, without notes.
[[nodiscard]] GeneratedTrack styleTemplate(std::string_view style,
                                           const preset::PresetLibrary& presets,
                                           const StyleOptions& options = {});

/// A whole track in `style`. Throws std::invalid_argument for an unknown style.
[[nodiscard]] GeneratedTrack generateTrack(std::string_view style,
                                           const preset::PresetLibrary& presets,
                                           const StyleOptions& options = {});

} // namespace adx::generate
