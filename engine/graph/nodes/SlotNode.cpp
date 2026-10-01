#include "engine/graph/nodes/SlotNode.h"

#include <algorithm>

namespace adx::graph {

void SlotNode::prepare(const PrepareInfo& /*info*/) {}

void SlotNode::reset() noexcept {}

PortSpec SlotNode::ports() const noexcept {
    return PortSpec{.inputs = 1, .outputs = 1, .acceptsEvents = false};
}

bool SlotNode::processWet(std::span<const float> /*inLeft*/, std::span<const float> /*inRight*/,
                          std::span<float> /*outLeft*/, std::span<float> /*outRight*/,
                          std::span<const float> /*effectParams*/,
                          ProcessContext& /*context*/) noexcept {
    return false;
}

void SlotNode::process(ProcessContext& context) noexcept {
    const std::span<const float> inLeft = context.inputs[0];
    const std::span<const float> inRight = context.inputs[1];
    const std::span<float> outLeft = context.outputs[0];
    const std::span<float> outRight = context.outputs[1];

    const float mix = context.params[static_cast<std::uint32_t>(SlotParam::Mix)];
    const bool bypassed = context.params[static_cast<std::uint32_t>(SlotParam::Bypass)] >= 0.5F;
    const std::span<const float> effectParams = context.params.subspan(kSlotParamCount);

    if (bypassed || !processWet(inLeft, inRight, outLeft, outRight, effectParams, context)) {
        std::ranges::copy(inLeft, outLeft.begin());
        std::ranges::copy(inRight, outRight.begin());
        return;
    }
    if (mix >= 1.0F) {
        return; // fully wet: the effect's output is the slot's
    }
    const float dry = 1.0F - mix;
    for (std::size_t i = 0; i < outLeft.size(); ++i) {
        outLeft[i] = (inLeft[i] * dry) + (outLeft[i] * mix);
        outRight[i] = (inRight[i] * dry) + (outRight[i] * mix);
    }
}

} // namespace adx::graph
