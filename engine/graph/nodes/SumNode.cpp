#include "engine/graph/nodes/SumNode.h"

#include <algorithm>

namespace adx::graph {

void SumNode::prepare(const PrepareInfo& /*info*/) {}

void SumNode::reset() noexcept {}

PortSpec SumNode::ports() const noexcept {
    return PortSpec{.inputs = 1, .outputs = 1, .acceptsEvents = false};
}

void SumNode::process(ProcessContext& context) noexcept {
    std::ranges::copy(context.inputs[0], context.outputs[0].begin());
    std::ranges::copy(context.inputs[1], context.outputs[1].begin());
}

} // namespace adx::graph
