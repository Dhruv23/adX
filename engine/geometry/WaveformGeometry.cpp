#include "engine/geometry/WaveformGeometry.h"

#include <algorithm>
#include <cmath>

#include "engine/geometry/ColorPalette.h"

namespace adx::geometry {
namespace {

constexpr float kPendingHalfHeight = 0.004F;
constexpr float kFromInt16 = 1.0F / 32767.0F;

} // namespace

void WaveformGeometry::build(const ClipPeaks& peaks, const WaveView& view) {
    const std::uint32_t columns = view.columns;
    const std::uint32_t rate = peaks.sampleRate();
    if (columns == 0 || rate == 0 || view.frameEnd <= view.frameStart) {
        m_tier = -1;
        static_cast<void>(m_strip.resize(0, kFloatsPerVertex));
        return;
    }
    const double perColumn = (view.frameEnd - view.frameStart) / static_cast<double>(columns);
    m_tier = peaks.chooseTier(perColumn);
    const std::span<float> out = m_strip.resize(std::size_t{columns} * 2, kFloatsPerVertex);
    const double secondsPerFrame = 1.0 / static_cast<double>(rate);
    const float pending = roleValue(ColorRole::kWavePending);
    const float fill = roleValue(ColorRole::kWaveFill);
    const auto frames = static_cast<double>(peaks.frames());

    std::size_t w = 0;
    for (std::uint32_t c = 0; c < columns; ++c) {
        const double a = view.frameStart + (perColumn * static_cast<double>(c));
        const double b = a + perColumn;
        const auto x = static_cast<float>(a * secondsPerFrame);
        float lo = -kPendingHalfHeight;
        float hi = kPendingHalfHeight;
        float role = pending;
        if (m_tier >= 0 && b > 0.0 && a < frames) {
            const auto begin = static_cast<std::uint64_t>(std::max(0.0, std::floor(a)));
            const auto end =
                static_cast<std::uint64_t>(std::max(std::ceil(b), std::floor(a) + 1.0));
            const auto [qlo, qhi] = peaks.range(m_tier, begin, end);
            lo = static_cast<float>(qlo) * kFromInt16;
            hi = std::max(static_cast<float>(qhi) * kFromInt16, lo);
            role = fill;
        } else if (m_tier >= 0) {
            lo = 0.0F;
            hi = 0.0F;
            role = fill;
        }
        out[w++] = x;
        out[w++] = hi;
        out[w++] = role;
        out[w++] = x;
        out[w++] = lo;
        out[w++] = role;
    }
}

} // namespace adx::geometry
