// adx-thread: main
#include "engine/geometry/StepGridGeometry.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "engine/geometry/ColorPalette.h"

namespace adx::geometry {
namespace {

/// Writes one quad's six vertices at `out`, returning the position after them.
float* quad(float* out, float x0, float y0, float x1, float y1, ColorRole role) noexcept {
    const float r = roleValue(role);
    for (const float value : {x0, y0, r, x1, y0, r, x1, y1, r, x0, y0, r, x1, y1, r, x0, y1, r}) {
        *out++ = value;
    }
    return out;
}

constexpr float kInset = 0.08F;

} // namespace

void stepsOf(const project::Project& project, core::PatternId pattern, core::ChannelId channel,
             std::int64_t stepTicks, std::uint32_t steps, std::vector<StepCell>& out) {
    out.assign(steps, StepCell{});
    const project::Pattern* model = project.find(pattern);
    const project::NoteClip* clip = model == nullptr ? nullptr : model->clipFor(channel);
    if (clip == nullptr || stepTicks <= 0) {
        return;
    }
    for (const project::Note& note : clip->notes) {
        const std::int64_t step = (note.start.value + (stepTicks / 2)) / stepTicks;
        if (note.start.value < 0 || step < 0 || std::cmp_greater_equal(step, steps)) {
            continue;
        }
        StepCell& cell = out[static_cast<std::size_t>(step)];
        // Two notes on one step (a chord): the cell shows the loudest.
        if (!cell.note.valid() || note.velocity > cell.velocity) {
            cell = StepCell{.note = note.id, .velocity = note.velocity};
        }
    }
}

void StepGridGeometry::build(const project::Project& project, const StepGridSpec& spec) {
    const std::size_t cells = spec.channels.size() * spec.steps;
    const std::span<float> out = m_cells.resize(cells * 6, kFloatsPerVertex);
    float* write = out.data();
    m_lit = 0;
    // The cells first, remembering each one's velocity, so the bar buffer is sized once.
    std::vector<std::uint8_t>& velocities = m_velocities;
    velocities.assign(cells, 0);
    for (std::size_t row = 0; row < spec.channels.size(); ++row) {
        stepsOf(project, spec.pattern, spec.channels[row], spec.stepTicks, spec.steps, m_row);
        for (std::uint32_t step = 0; step < spec.steps; ++step) {
            const StepCell& cell = m_row[step];
            velocities[(row * spec.steps) + step] =
                cell.note.valid() ? std::max<std::uint8_t>(cell.velocity, 1) : 0;
            const bool groupB = spec.groupSize > 0 && (step / spec.groupSize) % 2 == 1;
            ColorRole role = groupB ? ColorRole::kStepGroup : ColorRole::kStepOff;
            if (cell.note.valid()) {
                role = ColorRole::kStepOn;
            }
            const auto x = static_cast<float>(step);
            const auto y = static_cast<float>(row);
            write = quad(write, x + kInset, y + kInset, x + 1.0F - kInset, y + 1.0F - kInset, role);
            m_lit += cell.note.valid() ? 1U : 0U;
        }
    }
    const std::span<float> bars = m_velocity.resize(m_lit * 6, kFloatsPerVertex);
    float* bar = bars.data();
    for (std::size_t index = 0; index < cells; ++index) {
        if (velocities[index] == 0) {
            continue;
        }
        const std::size_t row = index / spec.steps; // the channel row: whole rows
        const auto x = static_cast<float>(index % spec.steps);
        const auto y = static_cast<float>(row);
        const float bottom = y + 1.0F - (kInset * 2.0F);
        const float height =
            (1.0F - (kInset * 4.0F)) * static_cast<float>(velocities[index]) / 127.0F;
        bar = quad(bar, x + (kInset * 2.0F), bottom - height, x + 0.3F, bottom,
                   ColorRole::kStepVelocity);
    }
}

} // namespace adx::geometry
