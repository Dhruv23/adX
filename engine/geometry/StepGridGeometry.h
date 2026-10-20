// adx-thread: main
//
// The channel rack's step matrix (phase_6.md §4.2): channels down, steps across, a cell
// lit where a note starts on that step and a bar inside it for the note's velocity.
//
// C++ for the same reason as the piano roll: a 64 x 64 rack is 4,096 cells, and 4,096
// Python widgets is exactly what this architecture exists to avoid. The grid edits the
// selected pattern's notes at step resolution - the piano roll edits the same notes at
// full resolution - and neither keeps a copy, so the two cannot disagree.
//
// World space: x is the step index, y the row index. Buffers (three floats a vertex):
//
//   cells     triangles  every cell, off or lit, groups of steps shaded alternately
//   velocity  triangles  one bar per lit cell, its height the velocity
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/geometry/GeometryBuffer.h"
#include "engine/project/Project.h"

namespace adx::geometry {

struct StepGridSpec {
    core::PatternId pattern;
    /// One row per channel, top to bottom.
    std::span<const core::ChannelId> channels;
    std::int64_t stepTicks{core::kPpq / 4};
    std::uint32_t steps{16};
    /// Steps per shaded group (a beat of sixteenths is 4).
    std::uint32_t groupSize{4};
};

/// What one channel plays on one step: the note starting nearest it (within half a
/// step), or none.
struct StepCell {
    core::NoteId note;
    std::uint8_t velocity{0};
};

/// The cell of every step for `channel` in `pattern`: steps() entries, note invalid where
/// no note starts. A note belongs to the step its start is nearest, so a swung note -
/// pushed late by less than half a step - still lights its own step.
void stepsOf(const project::Project& project, core::PatternId pattern, core::ChannelId channel,
             std::int64_t stepTicks, std::uint32_t steps, std::vector<StepCell>& out);

class StepGridGeometry {
public:
    void build(const project::Project& project, const StepGridSpec& spec);

    [[nodiscard]] const GeometryBuffer& cells() const noexcept {
        return m_cells;
    }
    [[nodiscard]] const GeometryBuffer& velocity() const noexcept {
        return m_velocity;
    }
    [[nodiscard]] GeometryBuffer& buffer(std::size_t index) noexcept {
        return index == 0 ? m_cells : m_velocity;
    }
    /// Cells lit by the last build.
    [[nodiscard]] std::size_t lit() const noexcept {
        return m_lit;
    }

private:
    GeometryBuffer m_cells;
    GeometryBuffer m_velocity;
    std::vector<StepCell> m_row;
    std::vector<std::uint8_t> m_velocities;
    std::size_t m_lit{0};
};

} // namespace adx::geometry
