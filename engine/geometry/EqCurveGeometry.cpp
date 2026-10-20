// adx-thread: main
#include "engine/geometry/EqCurveGeometry.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"
#include "engine/effects/ParametricEq.h"
#include "engine/geometry/ColorPalette.h"

namespace adx::geometry {
namespace {

[[nodiscard]] dsp::BiquadKind kindOf(effects::EqBandType type) noexcept {
    switch (type) {
    case effects::EqBandType::LowShelf:
        return dsp::BiquadKind::LowShelf;
    case effects::EqBandType::HighShelf:
        return dsp::BiquadKind::HighShelf;
    case effects::EqBandType::LowCut:
        return dsp::BiquadKind::HighPass;
    case effects::EqBandType::HighCut:
        return dsp::BiquadKind::LowPass;
    case effects::EqBandType::Notch:
        return dsp::BiquadKind::Notch;
    case effects::EqBandType::BandPass:
        return dsp::BiquadKind::BandPass;
    case effects::EqBandType::Peak:
    case effects::EqBandType::Off:
        break;
    }
    return dsp::BiquadKind::Peaking;
}

void push(std::vector<float>& out, float x, float y, ColorRole role) {
    out.insert(out.end(), {x, y, roleValue(role)});
}

} // namespace

std::vector<EqBand> eqBandsOf(std::string_view type, std::span<const float> params) {
    std::vector<EqBand> bands;
    if (type == "ParametricEq") {
        for (std::uint32_t b = 0; b < effects::kEqBands; ++b) {
            if (effects::eqParam(b, 3) >= params.size()) {
                break;
            }
            const auto bandType = static_cast<effects::EqBandType>(
                std::clamp(static_cast<int>(std::lround(params[effects::eqParam(b, 0)])), 0, 7));
            if (bandType == effects::EqBandType::Off) {
                continue;
            }
            // As the effect designs it: a shelf's q is its slope, capped at 1.
            const bool shelf = bandType == effects::EqBandType::LowShelf ||
                               bandType == effects::EqBandType::HighShelf;
            const float q = params[effects::eqParam(b, 3)];
            bands.push_back(EqBand{.kind = kindOf(bandType),
                                   .frequency = params[effects::eqParam(b, 1)],
                                   .q = shelf ? std::min(q, 1.0F) : q,
                                   .gainDb = params[effects::eqParam(b, 2)]});
        }
    } else if (type == "EQ" && params.size() >= 3) {
        // v1's three bands at their fixed frequencies (effects/Ported.cpp, Eq3).
        bands.push_back(EqBand{
            .kind = dsp::BiquadKind::LowShelf, .frequency = 250.0, .q = 1.0, .gainDb = params[0]});
        bands.push_back(EqBand{
            .kind = dsp::BiquadKind::Peaking, .frequency = 1200.0, .q = 0.7, .gainDb = params[1]});
        bands.push_back(EqBand{.kind = dsp::BiquadKind::HighShelf,
                               .frequency = 4000.0,
                               .q = 1.0,
                               .gainDb = params[2]});
    }
    return bands;
}

float eqAxisPosition(double frequency) noexcept {
    const double f = std::clamp(frequency, kEqLowHz, kEqHighHz);
    return static_cast<float>(dsp::log2(f / kEqLowHz) / dsp::log2(kEqHighHz / kEqLowHz));
}

double eqAxisFrequency(double x) noexcept {
    return kEqLowHz * dsp::exp2(std::clamp(x, 0.0, 1.0) * dsp::log2(kEqHighHz / kEqLowHz));
}

GeometryBuffer& EqCurveGeometry::buffer(std::size_t index) noexcept {
    if (index == 0) {
        return m_curve;
    }
    return index == 1 ? m_handles : m_spectrum;
}

void EqCurveGeometry::build(std::span<const EqBand> bands, double sampleRate, float rangeDb,
                            std::uint32_t points) {
    points = std::max<std::uint32_t>(points, 2);
    const double range = std::max(1.0, static_cast<double>(rangeDb));
    const auto yOf = [range](double db) {
        return static_cast<float>(std::clamp(0.5 - (db / (2.0 * range)), 0.0, 1.0));
    };
    std::vector<dsp::BiquadCoefficients> designs;
    designs.reserve(bands.size());
    for (const EqBand& band : bands) {
        designs.push_back(
            dsp::biquadCoefficients(band.kind, band.frequency, band.q, band.gainDb, sampleRate));
    }
    m_response.assign(points, 0.0);
    m_scratch.clear();
    float previousX = 0.0F;
    float previousY = 0.0F;
    for (std::uint32_t i = 0; i < points; ++i) {
        const double x = static_cast<double>(i) / static_cast<double>(points - 1);
        const double frequency = std::min(eqAxisFrequency(x), sampleRate * 0.499);
        double db = 0.0;
        for (const dsp::BiquadCoefficients& design : designs) {
            db += dsp::biquadMagnitudeDb(design, frequency, sampleRate);
        }
        m_response[i] = db;
        const auto px = static_cast<float>(x);
        const float py = yOf(db);
        if (i > 0) {
            push(m_scratch, previousX, previousY, ColorRole::kEqCurve);
            push(m_scratch, px, py, ColorRole::kEqCurve);
        }
        previousX = px;
        previousY = py;
    }
    const std::span<float> curve =
        m_curve.resize(m_scratch.size() / kFloatsPerVertex, kFloatsPerVertex);
    std::ranges::copy(m_scratch, curve.begin());

    m_scratch.clear();
    constexpr float kHalf = 0.012F;
    for (const EqBand& band : bands) {
        const float x = eqAxisPosition(band.frequency);
        const bool gainless =
            band.kind == dsp::BiquadKind::LowPass || band.kind == dsp::BiquadKind::HighPass ||
            band.kind == dsp::BiquadKind::Notch || band.kind == dsp::BiquadKind::BandPass;
        const float y = yOf(gainless ? 0.0 : band.gainDb);
        for (const auto& [vx, vy] :
             {std::pair{x - kHalf, y - (kHalf * 2.0F)}, std::pair{x + kHalf, y - (kHalf * 2.0F)},
              std::pair{x + kHalf, y + (kHalf * 2.0F)}, std::pair{x - kHalf, y - (kHalf * 2.0F)},
              std::pair{x + kHalf, y + (kHalf * 2.0F)}, std::pair{x - kHalf, y + (kHalf * 2.0F)}}) {
            push(m_scratch, vx, vy, ColorRole::kAutomationPoint);
        }
    }
    const std::span<float> handles =
        m_handles.resize(m_scratch.size() / kFloatsPerVertex, kFloatsPerVertex);
    std::ranges::copy(m_scratch, handles.begin());
}

void EqCurveGeometry::buildSpectrum(std::span<const float> samples, double sampleRate) {
    std::size_t size = 1;
    while (size * 2 <= std::min<std::size_t>(samples.size(), 4096)) {
        size *= 2;
    }
    m_scratch.clear();
    if (size < 64) {
        static_cast<void>(m_spectrum.resize(0, kFloatsPerVertex));
        return;
    }
    if (m_fft.size() != size) {
        m_fft.prepare(size);
    }
    m_bins.resize(size);
    m_magnitudes.resize(size / 2);
    dsp::logMagnitudeSpectrum(m_fft, samples.last(size), m_bins, m_magnitudes);
    float previousX = -1.0F;
    float previousY = 1.0F;
    for (std::size_t bin = 1; bin < m_magnitudes.size(); ++bin) {
        const double frequency = static_cast<double>(bin) * sampleRate / static_cast<double>(size);
        if (frequency < kEqLowHz || frequency > kEqHighHz) {
            continue;
        }
        const float x = eqAxisPosition(frequency);
        // 0..1 from -100..0 dB: drawn from the bottom up.
        const float y = 1.0F - std::clamp(m_magnitudes[bin], 0.0F, 1.0F);
        if (previousX >= 0.0F) {
            push(m_scratch, previousX, previousY, ColorRole::kWavePending);
            push(m_scratch, x, y, ColorRole::kWavePending);
        }
        previousX = x;
        previousY = y;
    }
    const std::span<float> out =
        m_spectrum.resize(m_scratch.size() / kFloatsPerVertex, kFloatsPerVertex);
    std::ranges::copy(m_scratch, out.begin());
}

} // namespace adx::geometry
