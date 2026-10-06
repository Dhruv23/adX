// adx-thread: main
//
// Turns a viewport and one channel's notes in one pattern into vertices
// (phase_5.md §4.4). This is the O(notes) half of the piano roll; Python owns what the
// view is, and the scene graph owns drawing it.
//
// Three things keep it inside the budget (phase_5.md §4.9):
//   - culling by binary search into notes sorted by start, so cost follows the visible
//     notes, not the pattern;
//   - a density level of detail below ~2 px per sixteenth, where notes merge into one
//     run per pitch row instead of a quad each;
//   - every buffer is reused, so a steady-state rebuild allocates nothing.
//
// Buffers, all three floats per vertex (Viewport.h's world space, then a ColorRole):
//   rows    triangles  one band per pitch row, coloured by key and scale
//   grid    lines      bar, beat and grid-division lines
//   ghosts  triangles  every other channel's notes in the pattern, dimmed
//   notes   triangles  6 vertices a note (or a density run)
//   lanes   triangles  one bar per note: x in world space, y in 0..1 down the lane
//   curves  lines      slides and pitch curves, drawn over their notes
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/geometry/GeometryBuffer.h"
#include "engine/geometry/Viewport.h"
#include "engine/project/Pattern.h"
#include "engine/project/Project.h"

namespace adx::geometry {

/// Which per-note value the lane under the roll shows (phase_5.md §4.7).
enum class LaneKind : std::uint8_t {
    kVelocity,
    kPan,
    kCutoff,
    kResonance,
    kFinePitch,
    kReleaseVelocity,
};

/// A scale to highlight: `mask` bit n set means the pitch class root + n is in it.
/// A zero mask is no highlighting.
struct ScaleHighlight {
    std::uint8_t root{0};
    std::uint16_t mask{0};

    [[nodiscard]] bool contains(int pitch) const noexcept {
        const int degree = ((pitch - root) % 12 + 12) % 12;
        return (mask & (1U << static_cast<unsigned>(degree))) != 0U;
    }
};

struct BuildOptions {
    core::PatternId pattern;
    core::ChannelId channel;
    /// Sorted ascending by id value.
    std::span<const core::NoteId> selected;
    /// Channels drawn as ghosts. Empty means every other channel in the pattern.
    std::span<const core::ChannelId> ghostChannels;
    bool ghosts{true};
    LaneKind lane{LaneKind::kVelocity};
    ScaleHighlight scale;
    /// Grid division for the guide lines, in ticks; 0 is none.
    std::int64_t gridDivision{core::kPpq / 4};
    /// The project's change token (CommandStack::revision). The sorted note order is
    /// rebuilt only when this, the pattern or the channel changes.
    std::uint64_t dataRevision{0};
};

/// Lane bars are this wide, in pixels at the build's zoom, unless the note is shorter.
/// HitTest uses the same width, so a bar is clickable exactly where it is drawn.
inline constexpr float kLaneBarPixels = 5.0F;

/// Below this many pixels per sixteenth note, notes draw as density runs.
inline constexpr float kDensityPixelsPerSixteenth = 2.0F;

[[nodiscard]] inline bool densityLod(const Viewport& viewport) noexcept {
    return viewport.pixelsPerTick * static_cast<float>(core::kPpq / 4) < kDensityPixelsPerSixteenth;
}

/// A lane value normalised to 0..1, and whether the bar grows from the middle.
[[nodiscard]] float laneValue(const project::Note& note, LaneKind lane) noexcept;
[[nodiscard]] bool laneIsBipolar(LaneKind lane) noexcept;

class PianoRollGeometry {
public:
    PianoRollGeometry() = default;

    /// Main thread. Fills every buffer, culled to `viewport`.
    void build(const project::Project& project, const Viewport& viewport,
               const BuildOptions& options);

    /// Recolours the notes and lane bars of the last build for a new selection
    /// (sorted ascending by id value), in place. No vertex moves and nothing is
    /// rebuilt; the two buffers' revisions bump.
    void updateSelection(std::span<const core::NoteId> selected);

    [[nodiscard]] const GeometryBuffer& rows() const noexcept {
        return m_rows;
    }
    [[nodiscard]] const GeometryBuffer& grid() const noexcept {
        return m_grid;
    }
    [[nodiscard]] const GeometryBuffer& ghosts() const noexcept {
        return m_ghosts;
    }
    [[nodiscard]] const GeometryBuffer& notes() const noexcept {
        return m_notes;
    }
    [[nodiscard]] const GeometryBuffer& lanes() const noexcept {
        return m_lanes;
    }
    [[nodiscard]] const GeometryBuffer& curves() const noexcept {
        return m_curves;
    }
    [[nodiscard]] GeometryBuffer& buffer(std::size_t index) noexcept;

    /// The notes the last build emitted, in vertex order (empty in density mode).
    [[nodiscard]] std::span<const core::NoteId> visibleNotes() const noexcept {
        return m_visibleIds;
    }
    [[nodiscard]] bool density() const noexcept {
        return m_density;
    }

private:
    void sortIfStale(const project::NoteClip* clip, const BuildOptions& options);
    void buildRows(const Viewport& viewport, const BuildOptions& options);
    void buildGrid(const project::Project& project, const Viewport& viewport,
                   const BuildOptions& options);
    void buildGhosts(const project::Pattern& pattern, const Viewport& viewport,
                     const BuildOptions& options);
    void buildNotes(const project::NoteClip& clip, const Viewport& viewport,
                    const BuildOptions& options);
    void buildDensity(const project::NoteClip& clip, const Viewport& viewport);
    void buildLanes(const project::NoteClip& clip, const Viewport& viewport,
                    const BuildOptions& options);
    void buildDensityLanes(const project::NoteClip& clip, const Viewport& viewport,
                           const BuildOptions& options);
    void buildCurves(const project::NoteClip& clip);
    /// [first, last) of m_order whose notes can overlap [tickStart, tickEnd].
    [[nodiscard]] std::pair<std::size_t, std::size_t>
    visibleRange(const project::NoteClip& clip, const Viewport& viewport) const noexcept;

    GeometryBuffer m_rows;
    GeometryBuffer m_grid;
    GeometryBuffer m_ghosts;
    GeometryBuffer m_notes;
    GeometryBuffer m_lanes;
    GeometryBuffer m_curves;

    /// Indices into the clip's notes, sorted by start.
    std::vector<std::uint32_t> m_order;
    std::int64_t m_maxLength{0};
    core::PatternId m_sortedPattern;
    core::ChannelId m_sortedChannel;
    std::uint64_t m_sortedRevision{0};
    bool m_sortedValid{false};

    std::vector<core::NoteId> m_visibleIds;
    std::vector<std::uint32_t> m_visibleIndex;
    std::vector<std::uint8_t> m_visibleMuted;
    /// Scratch for the density pass: per pitch, the open run's bucket range.
    std::array<std::int64_t, 128> m_runStart{};
    std::array<std::int64_t, 128> m_runEnd{};
    bool m_density{false};
};

} // namespace adx::geometry
