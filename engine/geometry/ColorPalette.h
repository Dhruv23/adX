// adx-thread: main
//
// Semantic colour roles for geometry (phase_5.md §4.10).
//
// A geometry builder never writes a colour. It writes one of these indices into each
// vertex, and the theme resolves the index to RGBA when the vertices are uploaded to
// the scene graph. That is what lets a theme switch, or a selection change, avoid
// rebuilding geometry: the vertices stay put and only the lookup changes.
//
// app/adx/theme/tokens.py names every role, in this order, and
// tests/python/test_bridge_contract.py checks the two lists agree.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace adx::geometry {

enum class ColorRole : std::uint8_t {
    kBackground,
    kRowWhite,
    kRowBlack,
    kRowInScale,
    kRowRoot,
    kGridSub,
    kGridBeat,
    kGridBar,
    kNote,
    kNoteSelected,
    kNoteMuted,
    kGhost,
    kDensity,
    kLane,
    kLaneSelected,
    kCurve,
    kPlayhead,
    kWaveFill,
    kWavePending,
    kSelectionRect,
    kCount,
};

inline constexpr std::size_t kColorRoleCount = static_cast<std::size_t>(ColorRole::kCount);

/// The role as the float a vertex carries in its third component.
[[nodiscard]] constexpr float roleValue(ColorRole role) noexcept {
    return static_cast<float>(static_cast<std::uint8_t>(role));
}

/// The names tokens.py must use, index for index.
inline constexpr std::array<const char*, kColorRoleCount> kColorRoleNames = {
    "background", "row_white", "row_black", "row_in_scale", "row_root",
    "grid_sub",   "grid_beat", "grid_bar",  "note",         "note_selected",
    "note_muted", "ghost",     "density",   "lane",         "lane_selected",
    "curve",      "playhead",  "wave_fill", "wave_pending", "selection_rect",
};

} // namespace adx::geometry
