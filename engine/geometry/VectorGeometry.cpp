// adx-thread: main
#include "engine/geometry/VectorGeometry.h"

#include <algorithm>
#include <cmath>

#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/Vertices.h"

namespace adx::geometry {

float phaseCorrelation(std::span<const rt::StereoFrame> frames) noexcept {
    double lr = 0.0;
    double ll = 0.0;
    double rr = 0.0;
    for (const rt::StereoFrame& f : frames) {
        lr += static_cast<double>(f.left) * f.right;
        ll += static_cast<double>(f.left) * f.left;
        rr += static_cast<double>(f.right) * f.right;
    }
    if (ll <= 1e-12 || rr <= 1e-12) {
        return 0.0F;
    }
    return static_cast<float>(std::clamp(lr / std::sqrt(ll * rr), -1.0, 1.0));
}

void VectorGeometry::build(std::span<const rt::StereoFrame> history, const VectorStyle& style) {
    const std::size_t window = std::min<std::size_t>(style.windowFrames, history.size());
    const std::span<const rt::StereoFrame> frames = history.subspan(history.size() - window);
    const float now = phaseCorrelation(frames);
    m_correlation = (style.smoothing * m_correlation) + ((1.0F - style.smoothing) * now);
    const auto point = [&](const rt::StereoFrame& f) {
        const float x = std::clamp((f.right - f.left) * 0.5F * style.gain, -1.0F, 1.0F);
        const float y = std::clamp(-(f.left + f.right) * 0.5F * style.gain, -1.0F, 1.0F);
        return std::pair{x, y};
    };
    writeVertices(m_trace, window > 1 ? (window - 1) * 2 : 0, [&](VertexWriter& w) {
        for (std::size_t i = 0; i + 1 < window; ++i) {
            const auto [x0, y0] = point(frames[i]);
            const auto [x1, y1] = point(frames[i + 1]);
            w.line(x0, y0, x1, y1, ColorRole::kVectorTrace);
        }
    });
    writeVertices(m_meter, 18, [&](VertexWriter& w) {
        // Below the trace, at y 1.1..1.25: the track, the centre mark, the reading.
        w.quad(-1.0F, 1.10F, 1.0F, 1.25F, ColorRole::kAnalyzerGrid);
        w.quad(-0.01F, 1.08F, 0.01F, 1.27F, ColorRole::kMeterBack);
        w.quad(std::min(0.0F, m_correlation), 1.12F, std::max(0.0F, m_correlation), 1.23F,
               ColorRole::kCorrelation);
    });
}

} // namespace adx::geometry
