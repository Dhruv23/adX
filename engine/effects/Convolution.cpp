#include "engine/effects/Convolution.h"

#include <algorithm>

#include "engine/dsp/Math.h"

namespace adx::effects {

Convolution::~Convolution() {
    destroyConvolutionPins(m_pins);
}

void Convolution::setImpulse(std::span<const float> left, std::span<const float> right,
                             ConvolutionPins* pins) noexcept {
    m_irLeft = left;
    m_irRight = right;
    destroyConvolutionPins(m_pins);
    m_pins = pins;
}

void Convolution::prepareEffect(const graph::PrepareInfo& /*info*/) {
    m_convolver[0].prepare(m_irLeft, kConvolutionPartition);
    m_convolver[1].prepare(m_irRight, kConvolutionPartition);
}

void Convolution::resetEffect() noexcept {
    for (dsp::Convolver& c : m_convolver) {
        c.reset();
    }
}

void Convolution::processWet(std::span<const float> inLeft, std::span<const float> inRight,
                             std::span<float> outLeft, std::span<float> outRight,
                             const EffectContext& context) noexcept {
    const std::uint32_t frames = context.frames;
    if (!m_convolver[0].ready() || !m_convolver[1].ready()) {
        std::fill_n(outLeft.begin(), frames, 0.0F);
        std::fill_n(outRight.begin(), frames, 0.0F);
        return;
    }
    m_convolver[0].process(inLeft.first(frames), outLeft.first(frames));
    m_convolver[1].process(inRight.first(frames), outRight.first(frames));
    for (std::uint32_t i = 0; i < frames; ++i) {
        const float gain = dsp::dbToGainF(context.paramAt(idx(ConvolutionParam::Gain), i));
        outLeft[i] *= gain;
        outRight[i] *= gain;
    }
}

} // namespace adx::effects
