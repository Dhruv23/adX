// adx-thread: main
#include "engine/geometry/SpectrogramGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace adx::geometry {

namespace {

constexpr double kLowHz = 20.0;
constexpr double kHighHz = 20000.0;

struct Rgb {
    float r;
    float g;
    float b;
};

/// Dark blue through magenta and orange to near white, by level 0..1.
Rgb ramp(float t) noexcept {
    constexpr std::array<Rgb, 5> kStops{{{.r = 0.02F, .g = 0.02F, .b = 0.08F},
                                         {.r = 0.20F, .g = 0.05F, .b = 0.45F},
                                         {.r = 0.70F, .g = 0.15F, .b = 0.55F},
                                         {.r = 0.98F, .g = 0.55F, .b = 0.15F},
                                         {.r = 1.00F, .g = 0.97F, .b = 0.85F}}};
    const float scaled = std::clamp(t, 0.0F, 1.0F) * static_cast<float>(kStops.size() - 1);
    const auto i = std::min(static_cast<std::size_t>(scaled), kStops.size() - 2);
    const float f = scaled - static_cast<float>(i);
    const Rgb& a = kStops.at(i);
    const Rgb& b = kStops.at(i + 1);
    return {
        .r = a.r + ((b.r - a.r) * f), .g = a.g + ((b.g - a.g) * f), .b = a.b + ((b.b - a.b) * f)};
}

std::uint8_t byte(float v) noexcept {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
}

} // namespace

void SpectrogramGeometry::configure(std::uint32_t rows, std::uint32_t columns, float floorDb) {
    m_rows = std::max<std::uint32_t>(1, rows);
    m_columns = std::max<std::uint32_t>(1, columns);
    m_floorDb = floorDb;
    m_head = m_rows - 1;
    m_pushed = 0;
    m_pixels.assign(static_cast<std::size_t>(m_rows) * m_columns * 4, 0);
    for (std::size_t i = 3; i < m_pixels.size(); i += 4) {
        m_pixels[i] = 255;
    }
    m_firstBin.assign(m_columns, 0);
    m_lastBin.assign(m_columns, 0);
    m_mappedBins = 0;
    m_mappedRate = 0;
}

void SpectrogramGeometry::push(std::span<const float> db, std::uint32_t sampleRate) noexcept {
    if (m_rows == 0 || db.size() < 2) {
        return;
    }
    if (db.size() != m_mappedBins || sampleRate != m_mappedRate) {
        // Remapped in place: the vectors were sized for the columns at configure().
        const double binHz =
            static_cast<double>(sampleRate) / (2.0 * static_cast<double>(db.size() - 1));
        for (std::uint32_t c = 0; c < m_columns; ++c) {
            const double from =
                kLowHz * std::pow(kHighHz / kLowHz, static_cast<double>(c) / m_columns);
            const double to =
                kLowHz * std::pow(kHighHz / kLowHz, static_cast<double>(c + 1) / m_columns);
            const auto first = std::min(static_cast<std::size_t>(from / binHz), db.size() - 1);
            const auto last =
                std::clamp(static_cast<std::size_t>(to / binHz), first, db.size() - 1);
            m_firstBin[c] = static_cast<std::uint32_t>(first);
            m_lastBin[c] = static_cast<std::uint32_t>(last);
        }
        m_mappedBins = db.size();
        m_mappedRate = sampleRate;
    }
    m_head = (m_head + 1) % m_rows;
    std::uint8_t* row = m_pixels.data() + (static_cast<std::size_t>(m_head) * m_columns * 4);
    for (std::uint32_t c = 0; c < m_columns; ++c) {
        float value = m_floorDb;
        for (std::uint32_t k = m_firstBin[c]; k <= m_lastBin[c]; ++k) {
            value = std::max(value, db[k]);
        }
        const Rgb rgb = ramp(1.0F - (value / m_floorDb));
        row[(c * 4) + 0] = byte(rgb.r);
        row[(c * 4) + 1] = byte(rgb.g);
        row[(c * 4) + 2] = byte(rgb.b);
        row[(c * 4) + 3] = 255;
    }
    ++m_pushed;
}

} // namespace adx::geometry
