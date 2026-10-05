#include "engine/effects/Drive.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::effects {
namespace {

/// The tube's bias: how far off centre the curve is driven, which is what makes it
/// asymmetric and gives it even harmonics.
constexpr float kTubeBias = 0.25F;

[[nodiscard]] int stepped(const EffectContext& context, std::uint32_t index,
                          std::uint32_t frame) noexcept {
    return static_cast<int>(std::floor(context.paramAt(index, frame) + 0.5F));
}

} // namespace

float saturate(SaturationModel model, float x) noexcept {
    switch (model) {
    case SaturationModel::Tape:
        return x / std::sqrt(1.0F + (x * x));
    case SaturationModel::Transformer: {
        const float c = std::clamp(x, -1.0F, 1.0F);
        return 1.5F * (c - (c * c * c / 3.0F));
    }
    case SaturationModel::Tube:
        break;
    }
    // tanh(x + b) - tanh(b): zero in, zero out; the DC it makes on signal is blocked.
    return dsp::tanhF(x + kTubeBias) - dsp::tanhF(kTubeBias);
}

float overdriveShape(OverdriveMode mode, float x) noexcept {
    switch (mode) {
    case OverdriveMode::Tube:
        return saturate(SaturationModel::Tube, x);
    case OverdriveMode::Hard:
        return std::clamp(x, -1.0F, 1.0F);
    case OverdriveMode::Soft:
        break;
    }
    return dsp::tanhF(x);
}

float AdaaClip::process(float x) noexcept {
    // F(x) = x^2 / 2 inside [-1, 1], |x| - 1/2 outside: the clamp's antiderivative.
    const auto antiderivative = [](float v) {
        const float a = std::abs(v);
        return a <= 1.0F ? 0.5F * v * v : a - 0.5F;
    };
    const float previous = m_previous;
    m_previous = x;
    const float delta = x - previous;
    if (std::abs(delta) < 1e-5F) {
        return std::clamp(0.5F * (x + previous), -1.0F, 1.0F);
    }
    return (antiderivative(x) - antiderivative(previous)) / delta;
}

// --- Saturation ---------------------------------------------------------------------

void Saturation::prepareEffect(const graph::PrepareInfo& info) {
    dsp::prepareOversampler();
    // A 5 Hz DC blocker.
    m_dcPole = static_cast<float>(1.0 - (dsp::kTwoPi * 5.0 / info.sampleRate));
}

void Saturation::resetEffect() noexcept {
    for (dsp::Oversampler4x& o : m_oversampler) {
        o.reset();
    }
    m_dcIn = {};
    m_dcOut = {};
}

void Saturation::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                            std::span<float> outLeft, std::span<float> outRight,
                            const EffectContext& context) noexcept {
    const std::array<std::span<const float>, 2> in{inLeft, inRight};
    const std::array<std::span<float>, 2> out{outLeft, outRight};
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            m_model = static_cast<SaturationModel>(
                std::clamp(stepped(context, idx(SaturationParam::Model), i), 0, 2));
        }
        const float drive = dsp::dbToGainF(context.paramAt(idx(SaturationParam::Drive), i));
        const float output = dsp::dbToGainF(context.paramAt(idx(SaturationParam::Output), i));
        const SaturationModel model = m_model;
        for (std::size_t c = 0; c < 2; ++c) {
            const float shaped = m_oversampler[c].process(
                in[c][i] * drive, [model](float s) { return saturate(model, s); });
            // y = x - x[-1] + p y[-1]
            const float blocked = shaped - m_dcIn[c] + (m_dcPole * m_dcOut[c]);
            m_dcIn[c] = shaped;
            m_dcOut[c] = blocked;
            out[c][i] = blocked * output;
        }
    }
}

// --- Overdrive ----------------------------------------------------------------------

void Overdrive::prepareEffect(const graph::PrepareInfo& info) {
    dsp::prepareOversampler();
    m_blendStep = 1.0F / (0.005F * static_cast<float>(info.sampleRate));
}

void Overdrive::resetEffect() noexcept {
    for (dsp::Oversampler4x& o : m_oversampler) {
        o.reset();
    }
    for (std::size_t c = 0; c < 2; ++c) {
        m_tight[c].reset();
        m_tone[c].reset();
        m_clip[c].reset();
    }
    m_blend = 1.0F;
}

// NOLINTNEXTLINE(readability-function-size) - the pedal's signal path in order.
void Overdrive::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                           std::span<float> outLeft, std::span<float> outRight,
                           const EffectContext& context) noexcept {
    const std::array<std::span<const float>, 2> in{inLeft, inRight};
    const std::array<std::span<float>, 2> out{outLeft, outRight};
    const double limit = 0.45 * context.sampleRate;
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        if (context.control(i)) {
            const double tight =
                std::clamp(static_cast<double>(context.paramAt(idx(OverdriveParam::Tightness), i)),
                           10.0, limit);
            const double tone = std::clamp(
                static_cast<double>(context.paramAt(idx(OverdriveParam::Tone), i)), 100.0, limit);
            m_tightCoefficients = dsp::svfCoefficients(tight, 0.7071, context.sampleRate);
            m_toneCoefficients = dsp::svfCoefficients(tone, 0.7071, context.sampleRate);
            const auto mode = static_cast<OverdriveMode>(
                std::clamp(stepped(context, idx(OverdriveParam::Mode), i), 0, 2));
            if (mode != m_mode) {
                // Fade from wherever the last change had got to.
                m_from = m_blend >= 0.5F ? m_mode : m_from;
                m_mode = mode;
                m_blend = 0.0F;
            }
        }
        const float drive = dsp::dbToGainF(context.paramAt(idx(OverdriveParam::Drive), i));
        const float level = dsp::dbToGainF(context.paramAt(idx(OverdriveParam::Level), i));
        const OverdriveMode mode = m_mode;
        const OverdriveMode from = m_from;
        const float blend = m_blend;
        for (std::size_t c = 0; c < 2; ++c) {
            AdaaClip& clip = m_clip[c];
            const auto shape = [mode, from, blend, &clip](float s) {
                // The hard clip's ADAA state must advance every sample, used or not, so
                // switching into Hard does not start from a stale previous input.
                const float hard = clip.process(s);
                const float now = mode == OverdriveMode::Hard ? hard : overdriveShape(mode, s);
                if (blend >= 1.0F) {
                    return now;
                }
                const float before = from == OverdriveMode::Hard ? hard : overdriveShape(from, s);
                return before + ((now - before) * blend);
            };
            const float tightened =
                m_tight[c].process(in[c][i], m_tightCoefficients, dsp::SvfMode::HighPass);
            const float driven = m_oversampler[c].process(tightened * drive, shape);
            out[c][i] =
                m_tone[c].process(driven, m_toneCoefficients, dsp::SvfMode::LowPass) * level;
        }
        m_blend = std::min(1.0F, m_blend + m_blendStep);
    }
}

} // namespace adx::effects
