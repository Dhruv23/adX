// adx-thread: main
#include "engine/geometry/SpectrumGeometry.h"

#include <algorithm>
#include <cmath>

#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/Vertices.h"

namespace adx::geometry {

namespace {

constexpr double kLowHz = 20.0;
constexpr double kHighHz = 20000.0;

float yOf(float db, float floorDb) noexcept {
    return std::clamp(db / floorDb, 0.0F, 1.0F);
}

} // namespace

float SpectrumGeometry::xOf(double hz) noexcept {
    return static_cast<float>(std::log(std::max(hz, kLowHz) / kLowHz) / std::log(kHighHz / kLowHz));
}

void SpectrumGeometry::resetPeaks() noexcept {
    std::ranges::fill(m_peaks, -1000.0F);
}

void SpectrumGeometry::build(std::span<const float> db, std::uint32_t sampleRate, double dtSeconds,
                             const SpectrumStyle& style) {
    const std::uint32_t columns = std::max<std::uint32_t>(2, style.columns);
    if (m_columns.size() != columns) {
        m_columns.assign(columns, style.floorDb);
        m_peaks.assign(columns, -1000.0F);
    }
    const std::size_t bins = db.size();
    const double binHz =
        bins > 1 ? static_cast<double>(sampleRate) / (2.0 * static_cast<double>(bins - 1)) : 1.0;
    const double ratio = kHighHz / kLowHz;
    for (std::uint32_t c = 0; c < columns; ++c) {
        float value = style.floorDb;
        if (bins > 1) {
            const double from = kLowHz * std::pow(ratio, static_cast<double>(c) / columns);
            const double to = kLowHz * std::pow(ratio, static_cast<double>(c + 1) / columns);
            auto first = static_cast<std::size_t>(from / binHz);
            auto last = static_cast<std::size_t>(to / binHz);
            first = std::min(first, bins - 1);
            last = std::clamp(last, first, bins - 1);
            for (std::size_t k = first; k <= last; ++k) {
                value = std::max(value, db[k]);
            }
        }
        m_columns[c] = value;
        const float fallen = m_peaks[c] - static_cast<float>(style.peakFallDbPerSecond * dtSeconds);
        m_peaks[c] = std::max(value, fallen);
    }

    const float floorDb = style.floorDb;
    const auto x = [&](std::uint32_t c) {
        return (static_cast<float>(c) + 0.5F) / static_cast<float>(columns);
    };
    writeVertices(m_fill, static_cast<std::size_t>(columns - 1) * 6, [&](VertexWriter& w) {
        for (std::uint32_t c = 0; c + 1 < columns; ++c) {
            const float y0 = yOf(m_columns[c], floorDb);
            const float y1 = yOf(m_columns[c + 1], floorDb);
            // A trapezoid down to the floor, as two triangles.
            w.vertex(x(c), y0, ColorRole::kSpectrumFill);
            w.vertex(x(c + 1), y1, ColorRole::kSpectrumFill);
            w.vertex(x(c + 1), 1.0F, ColorRole::kSpectrumFill);
            w.vertex(x(c), y0, ColorRole::kSpectrumFill);
            w.vertex(x(c + 1), 1.0F, ColorRole::kSpectrumFill);
            w.vertex(x(c), 1.0F, ColorRole::kSpectrumFill);
        }
    });
    writeVertices(m_lines, static_cast<std::size_t>(columns - 1) * 4, [&](VertexWriter& w) {
        for (std::uint32_t c = 0; c + 1 < columns; ++c) {
            w.line(x(c), yOf(m_columns[c], floorDb), x(c + 1), yOf(m_columns[c + 1], floorDb),
                   ColorRole::kSpectrumLine);
        }
        if (style.peakHold) {
            for (std::uint32_t c = 0; c + 1 < columns; ++c) {
                w.line(x(c), yOf(m_peaks[c], floorDb), x(c + 1), yOf(m_peaks[c + 1], floorDb),
                       ColorRole::kSpectrumPeak);
            }
        }
    });
}

} // namespace adx::geometry
