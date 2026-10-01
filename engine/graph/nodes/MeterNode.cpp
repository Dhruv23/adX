#include "engine/graph/nodes/MeterNode.h"

#include <algorithm>
#include <cmath>

namespace adx::graph {

void MeterNode::prepare(const PrepareInfo& /*info*/) {}

void MeterNode::reset() noexcept {}

PortSpec MeterNode::ports() const noexcept {
    return PortSpec{.inputs = 1, .outputs = 0, .acceptsEvents = false};
}

void MeterNode::process(ProcessContext& context) noexcept {
    const std::span<const float> left = context.inputs[0];
    const std::span<const float> right = context.inputs[1];

    float peakLeft = 0.0F;
    float peakRight = 0.0F;
    double squaresLeft = 0.0;
    double squaresRight = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        peakLeft = std::max(peakLeft, std::abs(left[i]));
        peakRight = std::max(peakRight, std::abs(right[i]));
        squaresLeft += static_cast<double>(left[i]) * left[i];
        squaresRight += static_cast<double>(right[i]) * right[i];
    }
    const double inverse = left.empty() ? 0.0 : 1.0 / static_cast<double>(left.size());
    m_ring.write(rt::LevelFrame{.peakLeft = peakLeft,
                                .peakRight = peakRight,
                                .rmsLeft = static_cast<float>(std::sqrt(squaresLeft * inverse)),
                                .rmsRight = static_cast<float>(std::sqrt(squaresRight * inverse))});
}

} // namespace adx::graph
