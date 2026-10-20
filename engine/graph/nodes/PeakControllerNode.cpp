#include "engine/graph/nodes/PeakControllerNode.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::graph {
namespace {

/// The one-pole coefficient that settles to 1 - 1/e in `ms`.
float coefficientFor(float ms, std::uint32_t sampleRate) noexcept {
    const double samples = std::max(1.0, static_cast<double>(ms) * 1e-3 * sampleRate);
    return static_cast<float>(dsp::exp(-1.0 / samples));
}

} // namespace

void PeakControllerNode::prepare(const PrepareInfo& info) {
    m_attack = coefficientFor(m_config.attackMs, info.sampleRate);
    m_release = coefficientFor(m_config.releaseMs, info.sampleRate);
    m_logRatio = m_config.logarithmic && m_config.minimum > 0.0F
                     ? dsp::log2(static_cast<double>(m_config.maximum) / m_config.minimum)
                     : 0.0;
    reset();
}

void PeakControllerNode::reset() noexcept {
    m_level = 0.0F;
    m_output = 0.0F;
}

PortSpec PeakControllerNode::ports() const noexcept {
    return PortSpec{.inputs = 1, .outputs = 0, .acceptsEvents = false};
}

void PeakControllerNode::process(ProcessContext& context) noexcept {
    const std::span<const float> left = context.inputs[0];
    const std::span<const float> right = context.inputs[1];
    float level = m_level;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const float in = std::max(std::abs(left[i]), std::abs(right[i])) * m_config.gain;
        const float coefficient = in > level ? m_attack : m_release;
        level = in + (coefficient * (level - in));
    }
    m_level = level;
    const double normalised = std::clamp(
        static_cast<double>(m_config.center + (m_config.depth * std::min(level, 1.0F))), 0.0, 1.0);
    const auto low = static_cast<double>(m_config.minimum);
    const auto high = static_cast<double>(m_config.maximum);
    m_output = static_cast<float>(m_logRatio != 0.0 ? low * dsp::exp2(normalised * m_logRatio)
                                                    : low + (normalised * (high - low)));
}

bool PeakControllerNode::modulation(float& out) const noexcept {
    out = m_output;
    return true;
}

} // namespace adx::graph
