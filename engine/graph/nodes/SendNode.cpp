#include "engine/graph/nodes/SendNode.h"

namespace adx::graph {

void SendNode::prepare(const PrepareInfo& /*info*/) {}

// The level a send last played at is not position state: a seek does not reset it.
void SendNode::reset() noexcept {}

PortSpec SendNode::ports() const noexcept {
    return PortSpec{.inputs = 1, .outputs = 1, .acceptsEvents = false};
}

void SendNode::process(ProcessContext& context) noexcept {
    constexpr auto kLevel = static_cast<std::uint32_t>(SendParam::Level);
    const std::uint32_t frames = context.frames;
    if (paramMoves(context, kLevel)) {
        // Automated: the level per frame, as every other per-sample parameter (P3-3).
        for (std::uint32_t channel = 0; channel < kPortChannels; ++channel) {
            const std::span<const float> in = context.inputs[channel];
            const std::span<float> out = context.outputs[channel];
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i] = in[i] * paramAt(context, kLevel, i);
            }
        }
        m_level = paramAt(context, kLevel, frames == 0 ? 0 : frames - 1);
        m_hasLevel = true;
        return;
    }
    // A knob turn: ramp from the level the last block ended on to the new one across
    // this block, so a send move does not zipper. A level that did not move multiplies
    // by exactly that level, as it always has.
    const float target = context.params[kLevel];
    const float start = m_hasLevel ? m_level : target;
    const float step = frames == 0 ? 0.0F : (target - start) / static_cast<float>(frames);
    for (std::uint32_t channel = 0; channel < kPortChannels; ++channel) {
        const std::span<const float> in = context.inputs[channel];
        const std::span<float> out = context.outputs[channel];
        if (step == 0.0F) {
            for (std::uint32_t i = 0; i < frames; ++i) {
                out[i] = in[i] * target;
            }
            continue;
        }
        for (std::uint32_t i = 0; i < frames; ++i) {
            out[i] = in[i] * (start + (step * static_cast<float>(i + 1)));
        }
    }
    m_level = target;
    m_hasLevel = true;
}

} // namespace adx::graph
