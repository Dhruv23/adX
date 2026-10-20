// adx-thread: main
#include "engine/geometry/MeterGeometry.h"

#include <algorithm>
#include <array>
#include <utility>

#include "engine/dsp/Math.h"
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

/// Quads per strip: background, then per channel the RMS bar, its hot part, the peak
/// marker - and the clip indicator.
constexpr std::size_t kQuadsPerStrip = 1 + (2 * 3) + 1;

} // namespace

float meterPosition(float amplitude) noexcept {
    if (!(amplitude > 0.0F)) {
        return 1.0F;
    }
    const auto db = static_cast<float>(dsp::gainToDb(static_cast<double>(amplitude)));
    return std::clamp(db / kMeterFloorDb, 0.0F, 1.0F);
}

void MeterGeometry::resetClips() noexcept {
    std::ranges::fill(m_clipped, std::uint8_t{0});
}

void MeterGeometry::build(std::span<const rt::LevelFrame> strips) {
    if (m_clipped.size() < strips.size()) {
        m_clipped.resize(strips.size(), 0);
    }
    const std::span<float> out =
        m_bars.resize(strips.size() * kQuadsPerStrip * 6, kFloatsPerVertex);
    float* write = out.data();
    const float hot = meterPosition(static_cast<float>(dsp::dbToGain(kMeterHotDb)));
    for (std::size_t i = 0; i < strips.size(); ++i) {
        const rt::LevelFrame& frame = strips[i];
        const auto x = static_cast<float>(i);
        write = quad(write, x + 0.30F, 0.0F, x + 0.70F, 1.0F, ColorRole::kMeterBack);
        const std::array<std::pair<float, float>, 2> channels{
            std::pair{frame.rmsLeft, frame.peakLeft}, std::pair{frame.rmsRight, frame.peakRight}};
        for (std::size_t c = 0; c < channels.size(); ++c) {
            const float left = x + 0.32F + (0.19F * static_cast<float>(c));
            const float right = left + 0.17F;
            const float rms = meterPosition(channels.at(c).first);
            const float peak = meterPosition(channels.at(c).second);
            // The bar grows from the bottom: the part below -6 dB in the RMS colour,
            // anything above it hot. Degenerate quads (zero height) keep the vertex count
            // fixed, so the buffer never reallocates as levels move.
            write = quad(write, left, std::max(rms, hot), right, 1.0F, ColorRole::kMeterRms);
            write = quad(write, left, rms, right, std::max(rms, hot), ColorRole::kMeterHot);
            write = quad(write, left, peak, right, std::min(peak + 0.008F, 1.0F),
                         ColorRole::kMeterPeak);
        }
        if (frame.peakLeft >= 1.0F || frame.peakRight >= 1.0F || frame.truePeak > 1.0F) {
            m_clipped[i] = 1;
        }
        const float clipBottom = m_clipped[i] != 0 ? 0.025F : 0.0F;
        write = quad(write, x + 0.30F, 0.0F, x + 0.70F, clipBottom, ColorRole::kMeterClip);
    }
}

} // namespace adx::geometry
