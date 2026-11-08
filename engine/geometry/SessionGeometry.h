// adx-thread: main
//
// The session grid (phase_11.md §4.9): tracks across, scenes down, each cell coloured by
// its clip's state, a progress ring on the playing cell of each track and a mark on a
// queued one. Built in C++ straight from the session's state atomics - an 8 x 64 grid is
// 512 cells, read and drawn in one build, one call from Python a frame.
//
// World space: x is the track, y the scene row, one unit a cell. Buffers (three floats
// a vertex, triangles):
//
//   cells  every cell, by look: empty, stopped, queued, playing, queued-stop, recording
//   marks  the progress ring of each playing cell, the queue mark of each queued one
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/geometry/GeometryBuffer.h"
#include "engine/session/SessionEngine.h"

namespace adx::geometry {

struct SessionGridSpec {
    std::uint32_t tracks{0};
    std::uint32_t rows{0};
    /// tracks x rows, track-major: non-zero where a cell holds a clip.
    std::span<const std::uint8_t> filled;
    /// Playing cells are drawn as recording while the performance log is armed.
    bool recording{false};
};

class SessionGeometry {
public:
    static constexpr std::uint32_t kRingSegments = 24;

    void build(const session::SessionEngine& session, const SessionGridSpec& spec);

    [[nodiscard]] GeometryBuffer& buffer(std::size_t index) noexcept {
        return index == 0 ? m_cells : m_marks;
    }
    [[nodiscard]] const GeometryBuffer& cells() const noexcept {
        return m_cells;
    }
    [[nodiscard]] const GeometryBuffer& marks() const noexcept {
        return m_marks;
    }
    /// What the last build found: cells playing and queued.
    [[nodiscard]] std::uint32_t playing() const noexcept {
        return m_playing;
    }
    [[nodiscard]] std::uint32_t queued() const noexcept {
        return m_queued;
    }

private:
    GeometryBuffer m_cells;
    GeometryBuffer m_marks;
    /// Each cell's state as the first pass read it: both passes see the same grid.
    std::vector<std::uint8_t> m_states;
    std::uint32_t m_playing{0};
    std::uint32_t m_queued{0};
};

} // namespace adx::geometry
