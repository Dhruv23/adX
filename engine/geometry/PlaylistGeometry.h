// adx-thread: main
//
// The playlist's vertices (phase_6.md §4.1): track lanes, the bar grid, every visible
// clip, and what is drawn inside one - a miniature of a pattern's notes, an audio clip's
// waveform, an automation clip's curve.
//
// The second hot surface, on the piano roll's pipeline: this builder, a GeometryBuffer
// per layer, the scene-graph item, a transform for pan and zoom. Level of detail matters
// more here than in the roll - a ten-minute arrangement zoomed out is hundreds of clips -
// so below kClipDetailPixels of width a clip is one flat rectangle and nothing else.
//
// World space: x is beats (ticks / PPQ); y is track rows, where a track of height h
// takes h rows and a collapsed group's members take none. Buffers, three floats a
// vertex (x, y, ColorRole):
//
//   lanes     triangles  one band per visible track, alternating
//   grid      lines      bar lines, and beat lines when there is room
//   clips     triangles  each clip's body and header strip
//   previews  triangles  notes in pattern clips, waveform columns in audio clips
//   curves    lines      automation clips' curves
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/geometry/GeometryBuffer.h"
#include "engine/project/Project.h"

namespace adx::geometry {

class ClipPeakCache;

/// Below this many pixels of width a clip draws as a flat rectangle: no header, no
/// preview (phase_6.md §4.1: "below ~30 px").
inline constexpr float kClipDetailPixels = 30.0F;
/// Within this many pixels of a clip's end, a press grabs the edge.
inline constexpr float kClipEdgePixels = 6.0F;
/// The header strip's share of a row.
inline constexpr float kClipHeaderShare = 0.22F;
/// At most this many notes are drawn inside one clip.
inline constexpr std::size_t kMaxPreviewNotes = 4096;

struct PlaylistViewport {
    double tickStart{0.0};
    double tickEnd{0.0};
    float rowStart{0.0F};
    float rowEnd{16.0F};
    float pixelsPerTick{0.01F};
    float pixelsPerRow{48.0F};

    [[nodiscard]] double tickAt(float x) const noexcept {
        return tickStart + static_cast<double>(x / pixelsPerTick);
    }
    [[nodiscard]] float rowAt(float y) const noexcept {
        return rowStart + (y / pixelsPerRow);
    }
};

struct PlaylistBuildOptions {
    /// Sorted ascending by id value.
    std::span<const core::ItemId> selected;
    /// Where the waveforms come from; null draws audio clips as plain rectangles.
    ClipPeakCache* peaks{nullptr};
};

/// One track's place in world space.
struct TrackRow {
    core::PlaylistTrackId track;
    float top{0.0F};
    float height{0.0F};
};

/// What content a clip holds.
enum class ClipKind : std::uint8_t { Pattern, Audio, Automation };

/// One clip the last build drew, for labels and hit tests.
struct VisibleClip {
    core::ItemId item;
    core::PlaylistTrackId track;
    std::int64_t start{0};
    std::int64_t end{0};
    float top{0.0F};
    float height{0.0F};
    ClipKind kind{ClipKind::Pattern};
    /// The pattern, sample or automation clip it refers to.
    std::uint32_t ref{0};
};

enum class ClipPart : std::uint8_t { None, Body, LeftEdge, RightEdge, Header };

struct ClipHit {
    core::PlaylistTrackId track;
    core::ItemId item;
    ClipPart part{ClipPart::None};
};

/// The rows every track takes: a collapsed group's members take none.
void layoutTracks(const project::Playlist& playlist, std::vector<TrackRow>& out);

class PlaylistGeometry {
public:
    void build(const project::Project& project, const PlaylistViewport& viewport,
               const PlaylistBuildOptions& options);

    [[nodiscard]] GeometryBuffer& buffer(std::size_t index) noexcept;
    [[nodiscard]] const GeometryBuffer& lanes() const noexcept {
        return m_lanes;
    }
    [[nodiscard]] const GeometryBuffer& grid() const noexcept {
        return m_grid;
    }
    [[nodiscard]] const GeometryBuffer& clips() const noexcept {
        return m_clips;
    }
    [[nodiscard]] const GeometryBuffer& previews() const noexcept {
        return m_previews;
    }
    [[nodiscard]] const GeometryBuffer& curves() const noexcept {
        return m_curves;
    }

    [[nodiscard]] std::span<const TrackRow> rows() const noexcept {
        return m_rows;
    }
    [[nodiscard]] std::span<const VisibleClip> visible() const noexcept {
        return m_visible;
    }
    /// True when the last build drew an audio clip whose peaks were not ready yet: build
    /// again when they are.
    [[nodiscard]] bool pendingPeaks() const noexcept {
        return m_pending;
    }
    /// Clips the last build drew with detail (header and preview).
    [[nodiscard]] std::size_t detailed() const noexcept {
        return m_detailed;
    }

    /// The clip under pixel (x, y) of the last build's viewport, and which part of it.
    [[nodiscard]] ClipHit hitTest(const PlaylistViewport& viewport, float x,
                                  float y) const noexcept;
    /// The track whose row is under pixel row y, or invalid.
    [[nodiscard]] core::PlaylistTrackId trackAt(const PlaylistViewport& viewport,
                                                float y) const noexcept;

private:
    void buildLanes(const PlaylistViewport& viewport);
    void buildGrid(const project::Project& project, const PlaylistViewport& viewport);
    void buildClip(const project::Project& project, const PlaylistViewport& viewport,
                   const project::PlaylistItem& item, const TrackRow& row, bool selected,
                   bool audible, const PlaylistBuildOptions& options);
    void previewPattern(const project::Pattern& pattern, const project::PlaylistItem& item,
                        std::int64_t end, float top, float height,
                        const PlaylistViewport& viewport);
    void previewAudio(const project::Project& project, const project::PlaylistItem& item,
                      std::int64_t end, float top, float height, const PlaylistViewport& viewport,
                      ClipPeakCache& peaks);
    void previewAutomation(const project::Project& project, const project::PlaylistItem& item,
                           std::int64_t end, float top, float height,
                           const PlaylistViewport& viewport);

    GeometryBuffer m_lanes;
    GeometryBuffer m_grid;
    GeometryBuffer m_clips;
    GeometryBuffer m_previews;
    GeometryBuffer m_curves;
    std::array<std::vector<float>, 5> m_scratch;
    std::vector<TrackRow> m_rows;
    std::vector<VisibleClip> m_visible;
    bool m_pending{false};
    std::size_t m_detailed{0};
};

} // namespace adx::geometry
