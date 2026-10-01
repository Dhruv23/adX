#include "engine/graph/nodes/SendNode.h"

namespace adx::graph {

void SendNode::prepare(const PrepareInfo& /*info*/) {}

void SendNode::reset() noexcept {}

PortSpec SendNode::ports() const noexcept {
    return PortSpec{.inputs = 1, .outputs = 1, .acceptsEvents = false};
}

void SendNode::process(ProcessContext& context) noexcept {
    const float level = context.params[static_cast<std::uint32_t>(SendParam::Level)];
    for (std::uint32_t channel = 0; channel < kPortChannels; ++channel) {
        const std::span<const float> in = context.inputs[channel];
        const std::span<float> out = context.outputs[channel];
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = in[i] * level;
        }
    }
}

} // namespace adx::graph
