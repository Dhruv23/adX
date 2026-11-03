// adx-thread: main
#include "engine/geometry/LoudnessGeometry.h"

#include <algorithm>

#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/Vertices.h"

namespace adx::geometry {

void LoudnessGeometry::reset() noexcept {
    std::ranges::fill(m_ring, -200.0F);
    m_head = 0;
    m_filled = 0;
}

void LoudnessGeometry::build(const rt::LevelFrame& master, const LoudnessStyle& style) {
    const std::size_t capacity = std::max<std::uint32_t>(2, style.historyFrames);
    if (m_ring.size() != capacity) {
        m_ring.assign(capacity, -200.0F);
        m_head = 0;
        m_filled = 0;
    }
    m_last = master;
    m_ring[m_head] = master.shortTerm;
    m_head = (m_head + 1) % capacity;
    m_filled = std::min(capacity, m_filled + 1);

    const float floor = style.floorLufs;
    const auto yOf = [floor](float lufs) { return std::clamp(lufs / floor, 0.0F, 1.0F); };
    writeVertices(m_bars, 6 * 6, [&](VertexWriter& w) {
        const float values[] = {master.momentary, master.shortTerm, master.integrated};
        for (int i = 0; i < 3; ++i) {
            const float x0 = 0.02F + (0.1F * static_cast<float>(i));
            const float x1 = x0 + 0.08F;
            w.quad(x0, 0.0F, x1, 1.0F, ColorRole::kMeterBack);
            w.quad(x0, yOf(values[i]), x1, 1.0F,
                   i == 2 ? ColorRole::kCorrelation : ColorRole::kLoudness);
        }
    });
    writeVertices(m_history, capacity * 2, [&](VertexWriter& w) {
        const auto x = [&](std::size_t age) {
            return 1.0F - (0.65F * static_cast<float>(age) / static_cast<float>(capacity - 1));
        };
        for (std::size_t age = 0; age + 1 < m_filled; ++age) {
            const float a = m_ring[(m_head + capacity - 1 - age) % capacity];
            const float b = m_ring[(m_head + capacity - 2 - age) % capacity];
            if (a <= -199.0F || b <= -199.0F) {
                continue;
            }
            w.line(x(age), yOf(a), x(age + 1), yOf(b), ColorRole::kLoudness);
        }
    });
}

} // namespace adx::geometry
