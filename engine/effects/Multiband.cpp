#include "engine/effects/Multiband.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

constexpr double kButterworthQ = 0.70710678118654752;
/// The soft knee's width, dB.
constexpr float kKnee = 6.0F;

/// Gain reduction in dB (<= 0) for a level `over` dB above threshold.
[[nodiscard]] float reductionDb(float over, float ratio) noexcept {
    const float slope = 1.0F - (1.0F / std::max(1.0F, ratio));
    if (over <= -kKnee * 0.5F) {
        return 0.0F;
    }
    if (over < kKnee * 0.5F) {
        const float x = over + (kKnee * 0.5F);
        return -slope * x * x / (2.0F * kKnee);
    }
    return -slope * over;
}

/// One-pole coefficient reaching 1 - 1/e in `ms`.
[[nodiscard]] float pole(float ms, std::uint32_t rate) noexcept {
    const double samples = std::max(1.0, static_cast<double>(ms) * 0.001 * rate);
    return static_cast<float>(dsp::exp(-1.0 / samples));
}

} // namespace

// --- MultibandComp -------------------------------------------------------------------

void MultibandComp::resetEffect() noexcept {
    for (auto& channel : m_split) {
        for (Crossover& x : channel) {
            for (dsp::Biquad& f : x.low) {
                f.reset();
            }
            for (dsp::Biquad& f : x.high) {
                f.reset();
            }
        }
    }
    for (auto& channel : m_allpass) {
        for (auto& band : channel) {
            for (dsp::Biquad& f : band) {
                f.reset();
            }
        }
    }
    m_reduction = {};
}

void MultibandComp::refresh(const EffectContext& context, std::uint32_t frame) noexcept {
    m_bands = static_cast<std::size_t>(std::clamp(
        static_cast<int>(std::floor(context.paramAt(idx(MultibandParam::Bands), frame) + 0.5F)), 3,
        static_cast<int>(kMaxBands)));
    const double limit = 0.45 * context.sampleRate;
    double previous = 10.0;
    for (std::size_t x = 0; x + 1 < m_bands; ++x) {
        // Crossovers in ascending order, whatever the knobs say.
        const double hz = std::clamp(static_cast<double>(context.paramAt(
                                         static_cast<std::uint32_t>(MultibandParam::Crossover1) +
                                             static_cast<std::uint32_t>(x),
                                         frame)),
                                     previous * 1.01, limit);
        previous = hz;
        m_lowCoefficients[x] = dsp::biquadCoefficients(dsp::BiquadKind::LowPass, hz, kButterworthQ,
                                                       0.0, context.sampleRate);
        m_highCoefficients[x] = dsp::biquadCoefficients(dsp::BiquadKind::HighPass, hz,
                                                        kButterworthQ, 0.0, context.sampleRate);
        m_allpassCoefficients[x] = dsp::biquadCoefficients(dsp::BiquadKind::AllPass, hz,
                                                           kButterworthQ, 0.0, context.sampleRate);
    }
    m_attack = pole(context.paramAt(idx(MultibandParam::Attack), frame), context.sampleRate);
    m_release = pole(context.paramAt(idx(MultibandParam::Release), frame), context.sampleRate);
    for (std::size_t b = 0; b < kMaxBands; ++b) {
        m_ratio[b] = context.paramAt(bandParam(b, 1), frame);
    }
}

// NOLINTNEXTLINE(readability-function-size) - split, detect, compress, sum.
void MultibandComp::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                               std::span<float> outLeft, std::span<float> outRight,
                               const EffectContext& context) noexcept {
    const std::array<std::span<const float>, 2> in{inLeft, inRight};
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            refresh(context, i);
        }
        std::array<std::array<float, kMaxBands>, 2> band{};
        for (std::size_t c = 0; c < 2; ++c) {
            float rest = in[c][i];
            for (std::size_t x = 0; x + 1 < m_bands; ++x) {
                Crossover& split = m_split[c][x];
                float low = split.low[0].process(rest, m_lowCoefficients[x]);
                low = split.low[1].process(low, m_lowCoefficients[x]);
                float high = split.high[0].process(rest, m_highCoefficients[x]);
                high = split.high[1].process(high, m_highCoefficients[x]);
                // The later crossovers' phase, which the bands above this one get.
                for (std::size_t later = x + 1; later + 1 < m_bands; ++later) {
                    low = m_allpass[c][x][later].process(low, m_allpassCoefficients[later]);
                }
                band[c][x] = low;
                rest = high;
            }
            band[c][m_bands - 1] = rest;
        }
        float left = 0.0F;
        float right = 0.0F;
        for (std::size_t b = 0; b < m_bands; ++b) {
            // Stereo-linked peak detection, smoothed in dB.
            const float peak = std::max(std::abs(band[0][b]), std::abs(band[1][b]));
            const auto level = static_cast<float>(dsp::gainToDb(peak, -120.0));
            const float over = level - context.paramAt(bandParam(b, 0), i);
            const float target = reductionDb(over, m_ratio[b]);
            const float coefficient = target < m_reduction[b] ? m_attack : m_release;
            m_reduction[b] = target + (coefficient * (m_reduction[b] - target));
            const float gain = dsp::dbToGainF(m_reduction[b] + context.paramAt(bandParam(b, 2), i));
            left += band[0][b] * gain;
            right += band[1][b] * gain;
        }
        outLeft[i] = left;
        outRight[i] = right;
    }
}

// --- TransientShaper ----------------------------------------------------------------

void TransientShaper::prepareEffect(const graph::PrepareInfo& info) {
    m_fastAttack = pole(0.5F, info.sampleRate);
    m_fastRelease = pole(20.0F, info.sampleRate);
    m_slowAttack = pole(20.0F, info.sampleRate);
    m_slowRelease = pole(300.0F, info.sampleRate);
}

void TransientShaper::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                                 std::span<float> outLeft, std::span<float> outRight,
                                 const EffectContext& context) noexcept {
    constexpr float kFloor = 1e-6F;
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        const float level = std::max(std::abs(inLeft[i]), std::abs(inRight[i]));
        const float fastPole = level > m_fast ? m_fastAttack : m_fastRelease;
        const float slowPole = level > m_slow ? m_slowAttack : m_slowRelease;
        m_fast = level + (fastPole * (m_fast - level));
        m_slow = level + (slowPole * (m_slow - level));
        const auto difference =
            static_cast<float>(dsp::gainToDb((m_fast + kFloor) / (m_slow + kFloor)));
        const float attack =
            std::clamp(context.paramAt(idx(TransientParam::Attack), i), -1.0F, 1.0F);
        const float sustain =
            std::clamp(context.paramAt(idx(TransientParam::Sustain), i), -1.0F, 1.0F);
        const float gainDb = (attack * 0.5F * std::clamp(difference, 0.0F, 24.0F)) +
                             (sustain * 0.5F * std::clamp(-difference, 0.0F, 24.0F)) +
                             context.paramAt(idx(TransientParam::Output), i);
        const float gain = dsp::dbToGainF(gainDb);
        outLeft[i] = inLeft[i] * gain;
        outRight[i] = inRight[i] * gain;
    }
}

} // namespace adx::effects
