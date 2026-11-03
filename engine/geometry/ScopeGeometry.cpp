// adx-thread: main
#include "engine/geometry/ScopeGeometry.h"

#include <algorithm>

#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/Vertices.h"

namespace adx::geometry {

void ScopeGeometry::build(std::span<const rt::StereoFrame> history, const ScopeStyle& style) {
    if (style.hold && m_lines.vertexCount() > 0) {
        return;
    }
    const std::size_t window =
        std::min<std::size_t>(std::max<std::uint32_t>(2, style.windowFrames), history.size());
    if (window < 2) {
        (void)m_lines.resize(0, kFloatsPerVertex);
        return;
    }
    std::size_t start = history.size() - window;
    m_triggered = false;
    if (style.trigger && history.size() >= 2 * window) {
        // The latest rising crossing with a whole window after it.
        for (std::size_t i = history.size() - window; i > history.size() - (2 * window); --i) {
            if (history[i - 1].left < 0.0F && history[i].left >= 0.0F) {
                start = i;
                m_triggered = true;
                break;
            }
        }
    }
    m_lastStart = start;
    const auto span = static_cast<float>(window - 1);
    const auto yOf = [&](float v, float centre) {
        return centre - (std::clamp(v * style.gain, -1.0F, 1.0F) * 0.24F);
    };
    writeVertices(m_lines, (window - 1) * 4, [&](VertexWriter& w) {
        for (std::size_t i = 0; i + 1 < window; ++i) {
            const float x0 = static_cast<float>(i) / span;
            const float x1 = static_cast<float>(i + 1) / span;
            const rt::StereoFrame& a = history[start + i];
            const rt::StereoFrame& b = history[start + i + 1];
            w.line(x0, yOf(a.left, 0.25F), x1, yOf(b.left, 0.25F), ColorRole::kScopeLeft);
            w.line(x0, yOf(a.right, 0.75F), x1, yOf(b.right, 0.75F), ColorRole::kScopeRight);
        }
    });
}

} // namespace adx::geometry
