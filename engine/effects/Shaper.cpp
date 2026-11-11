#include "engine/effects/Shaper.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

constexpr double kButterworthQ = 0.70710678118654752;

[[nodiscard]] float pole(double ms, std::uint32_t rate) noexcept {
    const double samples = std::max(1.0, ms * 0.001 * rate);
    return static_cast<float>(dsp::exp(-1.0 / samples));
}

} // namespace

float shaperCurve(const std::array<float, kShaperKnots>& knots, bool smooth, float x) noexcept {
    constexpr auto kSegments = static_cast<float>(kShaperKnots - 1);
    const float position = (std::clamp(x, -1.0F, 1.0F) + 1.0F) * 0.5F * kSegments;
    const auto k = std::min(static_cast<std::size_t>(position), kShaperKnots - 2);
    const float t = position - static_cast<float>(k);
    const float y0 = knots[k];
    const float y1 = knots[k + 1];
    if (!smooth) {
        return y0 + ((y1 - y0) * t);
    }
    // Fritsch-Carlson: tangents from the neighbouring secants, zeroed at an extremum
    // and limited so the cubic never overshoots its knots. In knot-index units.
    const auto secant = [&knots](std::size_t i) { return knots[i + 1] - knots[i]; };
    const auto tangent = [&](std::size_t i) {
        if (i == 0) {
            return secant(0);
        }
        if (i == kShaperKnots - 1) {
            return secant(kShaperKnots - 2);
        }
        const float a = secant(i - 1);
        const float b = secant(i);
        if (a * b <= 0.0F) {
            return 0.0F;
        }
        return 2.0F / ((1.0F / a) + (1.0F / b)); // harmonic mean: monotone
    };
    const float m0 = tangent(k);
    const float m1 = tangent(k + 1);
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (((2.0F * t3) - (3.0F * t2) + 1.0F) * y0) + ((t3 - (2.0F * t2) + t) * m0) +
           (((-2.0F * t3) + (3.0F * t2)) * y1) + ((t3 - t2) * m1);
}

void Shaper::prepareEffect(const graph::PrepareInfo& info) {
    dsp::prepareOversampler();
    m_dcPole = static_cast<float>(1.0 - (2.0 * std::numbers::pi * 10.0 / info.sampleRate));
    resetEffect();
}

void Shaper::resetEffect() noexcept {
    for (dsp::Oversampler4x& over : m_over) {
        over.reset();
    }
    m_dcIn.fill(0.0F);
    m_dcOut.fill(0.0F);
}

void Shaper::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                        std::span<float> outLeft, std::span<float> outRight,
                        const EffectContext& context) noexcept {
    std::array<float, kShaperKnots> knots{};
    for (std::size_t k = 0; k < kShaperKnots; ++k) {
        knots[k] = std::clamp(
            context.param(idx(ShaperParam::Knot1) + static_cast<std::uint32_t>(k)), -1.0F, 1.0F);
    }
    const bool smooth = context.param(idx(ShaperParam::Smooth)) >= 0.5F;
    const bool dcBlock = context.param(idx(ShaperParam::DcBlock)) >= 0.5F;
    dsp::DbGain drive;
    dsp::DbGain output;
    const std::array<std::span<const float>, 2> in{inLeft, inRight};
    const std::array<std::span<float>, 2> out{outLeft, outRight};
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        const float gain = drive(context.paramAt(idx(ShaperParam::Drive), i));
        const float bias = context.paramAt(idx(ShaperParam::Bias), i);
        const float level = output(context.paramAt(idx(ShaperParam::Output), i));
        for (std::size_t c = 0; c < 2; ++c) {
            float y = m_over.at(c).process((in.at(c)[i] * gain) + bias, [&knots, smooth](float s) {
                return shaperCurve(knots, smooth, s);
            });
            if (dcBlock) {
                const float blocked = y - m_dcIn.at(c) + (m_dcPole * m_dcOut.at(c));
                m_dcIn.at(c) = y;
                m_dcOut.at(c) = blocked;
                y = blocked;
            }
            out.at(c)[i] = y * level;
        }
    }
}

// --- OTT ---------------------------------------------------------------------------

float ottGainDb(float level, float downThreshold, float downRatio, float upThreshold, float upRatio,
                float depth, float upward, float downward) noexcept {
    if (level > downThreshold) {
        const float slope = 1.0F - (1.0F / std::max(1.0F, downRatio));
        return -(level - downThreshold) * slope * depth * downward;
    }
    if (level < upThreshold) {
        const float slope = 1.0F - (1.0F / std::max(1.0F, upRatio));
        return std::min(kOttMaxUpwardDb, (upThreshold - level) * slope) * depth * upward;
    }
    return 0.0F;
}

void Ott::resetEffect() noexcept {
    for (auto& channel : m_split) {
        for (Split& split : channel) {
            for (dsp::Biquad& f : split.low) {
                f.reset();
            }
            for (dsp::Biquad& f : split.high) {
                f.reset();
            }
        }
    }
    for (dsp::Biquad& f : m_lowAllpass) {
        f.reset();
    }
    m_level.fill(0.0F);
}

void Ott::refresh(const EffectContext& context, std::uint32_t frame) noexcept {
    const double limit = 0.45 * context.sampleRate;
    const double low = std::clamp(
        static_cast<double>(context.paramAt(idx(OttParam::CrossoverLow), frame)), 20.0, limit);
    const double high =
        std::clamp(static_cast<double>(context.paramAt(idx(OttParam::CrossoverHigh), frame)),
                   low * 1.5, limit);
    const std::array<double, 2> hz{low, high};
    for (std::size_t x = 0; x < 2; ++x) {
        m_lowCoefficients[x] = dsp::biquadCoefficients(dsp::BiquadKind::LowPass, hz[x],
                                                       kButterworthQ, 0.0, context.sampleRate);
        m_highCoefficients[x] = dsp::biquadCoefficients(dsp::BiquadKind::HighPass, hz[x],
                                                        kButterworthQ, 0.0, context.sampleRate);
    }
    m_allpassCoefficients = dsp::biquadCoefficients(dsp::BiquadKind::AllPass, high, kButterworthQ,
                                                    0.0, context.sampleRate);
    // `time` 0..1: attack 0.1..30 ms and release 10..1000 ms, log-spaced together.
    const double time = std::clamp(context.paramAt(idx(OttParam::Time), frame), 0.0F, 1.0F);
    m_attack = pole(0.1 * dsp::exp2(time * dsp::log2(300.0)), context.sampleRate);
    m_release = pole(10.0 * dsp::exp2(time * dsp::log2(100.0)), context.sampleRate);
}

// NOLINTNEXTLINE(readability-function-size) - split, follow, gain, sum.
void Ott::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                     std::span<float> outLeft, std::span<float> outRight,
                     const EffectContext& context) noexcept {
    const float upward = std::clamp(context.param(idx(OttParam::Upward)), 0.0F, 1.0F);
    const float downward = std::clamp(context.param(idx(OttParam::Downward)), 0.0F, 1.0F);
    std::array<float, kOttBands> downRatio{};
    std::array<float, kOttBands> upRatio{};
    for (std::size_t b = 0; b < kOttBands; ++b) {
        downRatio[b] = context.param(ottBand(b, 1));
        upRatio[b] = context.param(ottBand(b, 3));
    }
    dsp::DbGain inGain;
    dsp::DbGain outGain;
    const std::array<std::span<const float>, 2> in{inLeft, inRight};
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            refresh(context, i);
        }
        const float input = inGain(context.paramAt(idx(OttParam::InGain), i));
        std::array<std::array<float, kOttBands>, 2> band{};
        for (std::size_t c = 0; c < 2; ++c) {
            const float x = in[c][i] * input;
            Split& first = m_split[c][0];
            float low = first.low[1].process(first.low[0].process(x, m_lowCoefficients[0]),
                                             m_lowCoefficients[0]);
            const float rest = first.high[1].process(
                first.high[0].process(x, m_highCoefficients[0]), m_highCoefficients[0]);
            Split& second = m_split[c][1];
            const float mid = second.low[1].process(
                second.low[0].process(rest, m_lowCoefficients[1]), m_lowCoefficients[1]);
            const float high = second.high[1].process(
                second.high[0].process(rest, m_highCoefficients[1]), m_highCoefficients[1]);
            // The low band gets the second crossover's phase, as the others have it.
            low = m_lowAllpass[c].process(low, m_allpassCoefficients);
            band[c] = {low, mid, high};
        }
        const float depth = std::clamp(context.paramAt(idx(OttParam::Depth), i), 0.0F, 1.0F);
        float left = 0.0F;
        float right = 0.0F;
        for (std::size_t b = 0; b < kOttBands; ++b) {
            // Stereo-linked peak follower.
            const float peak = std::max(std::abs(band[0][b]), std::abs(band[1][b]));
            const float coefficient = peak > m_level[b] ? m_attack : m_release;
            m_level[b] = peak + (coefficient * (m_level[b] - peak));
            const float levelDb = dsp::gainToDbF(m_level[b], -120.0F);
            const float gainDb =
                ottGainDb(levelDb, context.paramAt(ottBand(b, 0), i), downRatio[b],
                          context.paramAt(ottBand(b, 2), i), upRatio[b], depth, upward, downward) +
                context.paramAt(ottBand(b, 4), i);
            const float gain = dsp::dbToGainF(gainDb);
            left += band[0][b] * gain;
            right += band[1][b] * gain;
        }
        const float output = outGain(context.paramAt(idx(OttParam::OutGain), i));
        outLeft[i] = left * output;
        outRight[i] = right * output;
    }
}

} // namespace adx::effects
