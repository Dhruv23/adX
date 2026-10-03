#include "engine/effects/ParametricEq.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

[[nodiscard]] dsp::BiquadKind kindOf(EqBandType type) noexcept {
    switch (type) {
    case EqBandType::LowShelf:
        return dsp::BiquadKind::LowShelf;
    case EqBandType::HighShelf:
        return dsp::BiquadKind::HighShelf;
    case EqBandType::LowCut:
        return dsp::BiquadKind::HighPass;
    case EqBandType::HighCut:
        return dsp::BiquadKind::LowPass;
    case EqBandType::Notch:
        return dsp::BiquadKind::Notch;
    case EqBandType::BandPass:
        return dsp::BiquadKind::BandPass;
    case EqBandType::Peak:
    case EqBandType::Off:
        break;
    }
    return dsp::BiquadKind::Peaking;
}

} // namespace

void ParametricEq::resetEffect() noexcept {
    for (Band& band : m_bands) {
        band.left.reset();
        band.right.reset();
        band.last = {-1.0F, -1.0F, -1.0F, -1.0F};
    }
    m_frameFill = 0;
}

void ParametricEq::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                              std::span<float> outLeft, std::span<float> outRight,
                              const EffectContext& context) noexcept {
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            for (std::uint32_t b = 0; b < kEqBands; ++b) {
                Band& band = m_bands[b];
                std::array<float, kEqBandParams> now{};
                for (std::uint32_t f = 0; f < kEqBandParams; ++f) {
                    now[f] = context.paramAt(eqParam(b, f), i);
                }
                if (now == band.last) {
                    continue;
                }
                band.last = now;
                const auto type = static_cast<EqBandType>(
                    std::clamp(static_cast<int>(std::lround(now[0])), 0, 7));
                band.active = type != EqBandType::Off;
                if (band.active) {
                    // A shelf's q is its slope, which stops making sense above 1.
                    const bool shelf =
                        type == EqBandType::LowShelf || type == EqBandType::HighShelf;
                    const double q = shelf ? std::min(now[3], 1.0F) : now[3];
                    band.coefficients = dsp::biquadCoefficients(kindOf(type), now[1], q, now[2],
                                                                context.sampleRate);
                }
            }
        }
        float left = inLeft[i];
        float right = inRight[i];
        for (Band& band : m_bands) {
            if (band.active) {
                left = band.left.process(left, band.coefficients);
                right = band.right.process(right, band.coefficients);
            }
        }
        const float output = dsp::dbToGainF(context.paramAt(kEqOutputParam, i));
        left *= output;
        right *= output;
        outLeft[i] = left;
        outRight[i] = right;

        m_frame[m_frameFill++] = 0.5F * (left + right);
        if (m_frameFill == m_frame.size()) {
            m_tap.write(m_frame);
            m_frameFill = 0;
        }
    }
}

} // namespace adx::effects
