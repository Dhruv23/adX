#include "engine/effects/Clipper.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/dsp/Math.h"

namespace adx::effects {

float clipShape(ClipMode mode, float x) noexcept {
    switch (mode) {
    case ClipMode::Soft:
        return dsp::tanhF(x);
    case ClipMode::Hard:
        return std::clamp(x, -1.0F, 1.0F);
    case ClipMode::SineFold:
        // sin(x pi/2): rises to the ceiling at 1, then folds back down.
        return dsp::sinTurnsF(x * 0.25F);
    case ClipMode::Foldback: {
        // Reflect off +-1 until inside: a triangle of period 4.
        const float t = (x + 1.0F) * 0.25F;
        const float frac = t - dsp::floorExact(t);
        return frac < 0.5F ? (frac * 4.0F) - 1.0F : 3.0F - (frac * 4.0F);
    }
    case ClipMode::AsymTube:
        // Positive half harder than the negative: even harmonics, a DC offset the
        // blocker removes.
        return x >= 0.0F ? dsp::tanhF(x * 1.4F) / dsp::tanhF(1.4F)
                         : dsp::tanhF(x * 0.7F) / dsp::tanhF(0.7F);
    }
    return x;
}

void Clipper::prepareEffect(const graph::PrepareInfo& info) {
    dsp::prepareOversampler();
    m_dcPole = static_cast<float>(1.0 - (2.0 * std::numbers::pi * 10.0 / info.sampleRate));
    resetEffect();
}

void Clipper::resetEffect() noexcept {
    for (std::size_t c = 0; c < 2; ++c) {
        m_outer.at(c).reset();
        m_inner.at(c).reset();
    }
    m_dcIn.fill(0.0F);
    m_dcOut.fill(0.0F);
}

void Clipper::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                         std::span<float> outLeft, std::span<float> outRight,
                         const EffectContext& context) noexcept {
    const auto mode = static_cast<ClipMode>(std::clamp(
        static_cast<int>(dsp::floorExact(context.param(idx(ClipperParam::Mode)) + 0.5F)), 0, 4));
    const bool dcBlock = context.param(idx(ClipperParam::DcBlock)) >= 0.5F;
    dsp::DbGain drive;
    dsp::DbGain ceiling;
    const std::array<std::span<const float>, 2> in = {inLeft, inRight};
    const std::array<std::span<float>, 2> out = {outLeft, outRight};
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        const float gain = drive(context.paramAt(idx(ClipperParam::Drive), i));
        const float top = ceiling(context.paramAt(idx(ClipperParam::Ceiling), i));
        for (std::size_t c = 0; c < 2; ++c) {
            dsp::Oversampler4x& inner = m_inner.at(c);
            float y = m_outer.at(c).process(in.at(c)[i] * gain, [&inner, mode](float s) {
                return inner.process(s, [mode](float t) { return clipShape(mode, t); });
            });
            if (dcBlock) {
                const float blocked = y - m_dcIn.at(c) + (m_dcPole * m_dcOut.at(c));
                m_dcIn.at(c) = y;
                m_dcOut.at(c) = blocked;
                y = blocked;
            }
            out.at(c)[i] = y * top;
        }
    }
}

} // namespace adx::effects
