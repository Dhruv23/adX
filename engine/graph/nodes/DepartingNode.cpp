// adx-thread: realtime
#include "engine/graph/nodes/DepartingNode.h"

#include <algorithm>

namespace adx::graph {

DepartingNode::DepartingNode(ChannelNode& channel, std::uint32_t capFrames) noexcept
    : m_channel(&channel), m_remaining(capFrames) {}

void DepartingNode::prepare(const PrepareInfo& /*info*/) {}

PortSpec DepartingNode::ports() const noexcept {
    return PortSpec{.inputs = 0, .outputs = 1, .acceptsEvents = false};
}

void DepartingNode::process(ProcessContext& context) noexcept {
    if (done() || m_channel == nullptr) {
        for (const std::span<float> output : context.outputs) {
            std::ranges::fill(output, 0.0F);
        }
        return;
    }
    const bool sounding = m_channel->renderDeparture(context);
    m_remaining -= std::min(context.frames, m_remaining);
    if (!sounding || m_remaining == 0) {
        m_done.store(true, std::memory_order_release);
    }
}

} // namespace adx::graph
