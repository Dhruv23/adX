#include "engine/graph/nodes/MeterNode.h"

#include <algorithm>
#include <cmath>

namespace adx::graph {

void MeterNode::prepare(const PrepareInfo& info) {
    m_loudness.prepare(info.sampleRate);
}

// A seek does not clear a meter: loudness is a measurement of what was heard, and the
// integrated reading is reset by the transport's owner, not by a jump.
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
    m_loudness.process(left, right);
    float truePeak = 0.0F;
    if (m_truePeakEnabled) {
        for (std::size_t i = 0; i < left.size(); ++i) {
            truePeak = std::max({truePeak, m_peakLeft.push(left[i]), m_peakRight.push(right[i])});
        }
    }
    m_ring.write(rt::LevelFrame{.peakLeft = peakLeft,
                                .peakRight = peakRight,
                                .rmsLeft = static_cast<float>(std::sqrt(squaresLeft * inverse)),
                                .rmsRight = static_cast<float>(std::sqrt(squaresRight * inverse)),
                                .momentary = m_loudness.momentary(),
                                .shortTerm = m_loudness.shortTerm(),
                                .integrated = m_loudness.integrated(),
                                .truePeak = truePeak});
}

} // namespace adx::graph
