// adx-thread: realtime
#include "engine/plugin/PluginNode.h"

#include <algorithm>
#include <array>

namespace adx::plugin {

PluginEffect::PluginEffect(std::string_view format, const project::PluginRef& ref) {
    m_core.configure(format, ref, false);
}

void PluginEffect::prepare(const graph::PrepareInfo& info) {
    graph::SlotNode::prepare(info);
    m_core.prepare(info);
    m_ringLeft.allocate(kMaxDryDelay);
    m_ringRight.allocate(kMaxDryDelay);
    m_write = 0;
}

void PluginEffect::reset() noexcept {
    std::ranges::fill(m_ringLeft.view(), 0.0F);
    std::ranges::fill(m_ringRight.view(), 0.0F);
}

std::uint32_t PluginEffect::latencySamples() const noexcept {
    return std::min(m_core.latency(), kMaxDryDelay - 1);
}

void PluginEffect::delayDry(std::span<const float> inLeft, std::span<const float> inRight,
                            std::span<float> dryLeft, std::span<float> dryRight,
                            std::uint32_t delay) noexcept {
    const std::span<float> ringLeft = m_ringLeft.view();
    const std::span<float> ringRight = m_ringRight.view();
    const std::size_t size = ringLeft.size();
    if (size == 0) {
        std::ranges::copy(inLeft, dryLeft.begin());
        std::ranges::copy(inRight, dryRight.begin());
        return;
    }
    for (std::size_t i = 0; i < inLeft.size(); ++i) {
        ringLeft[m_write] = inLeft[i];
        ringRight[m_write] = inRight[i];
        const std::size_t read = (m_write + size - delay) % size;
        dryLeft[i] = ringLeft[read];
        dryRight[i] = ringRight[read];
        m_write = (m_write + 1) % size;
    }
}

void PluginEffect::processSlot(graph::ProcessContext& context) noexcept {
    const std::uint32_t frames = context.frames;
    const std::span<const float> inLeft = context.inputs[0];
    const std::span<const float> inRight = context.inputs[1];
    const std::span<float> outLeft = context.outputs[0];
    const std::span<float> outRight = context.outputs[1];
    const std::span<float> dryLeft = context.arena.allocate<float>(frames);
    const std::span<float> dryRight = context.arena.allocate<float>(frames);
    if (dryLeft.size() != frames || dryRight.size() != frames) {
        std::ranges::copy(inLeft, outLeft.begin());
        std::ranges::copy(inRight, outRight.begin());
        return;
    }
    // The dry path follows the plugin's latency as it is now, so wet and dry stay
    // aligned through a latency change (PDC catches up at the next rebuild).
    delayDry(inLeft, inRight, dryLeft, dryRight, latencySamplesLive());

    const float mix = context.params[static_cast<std::uint32_t>(graph::SlotParam::Mix)];
    const bool bypassed =
        context.params[static_cast<std::uint32_t>(graph::SlotParam::Bypass)] >= 0.5F;
    if (bypassed) {
        std::ranges::copy(dryLeft, outLeft.begin());
        std::ranges::copy(dryRight, outRight.begin());
        return;
    }
    const std::span<const float> params = context.params.subspan(graph::kSlotParamCount);
    const std::span<const float* const> automation =
        context.automation.size() > graph::kSlotParamCount
            ? context.automation.subspan(graph::kSlotParamCount)
            : std::span<const float* const>{};
    const std::array<const float*, 2> inputs{inLeft.data(), inRight.data()};
    const std::array<float*, 2> outputs{outLeft.data(), outRight.data()};
    const CoreResult result =
        m_core.process(inputs, outputs, frames, params, automation, {}, context.time);
    if (result == CoreResult::Absent) {
        std::ranges::copy(dryLeft, outLeft.begin());
        std::ranges::copy(dryRight, outRight.begin());
        return;
    }
    if (result == CoreResult::Silent || mix >= 1.0F) {
        return;
    }
    const float dry = 1.0F - std::max(mix, 0.0F);
    const float wet = std::max(mix, 0.0F);
    for (std::uint32_t i = 0; i < frames; ++i) {
        outLeft[i] = (dryLeft[i] * dry) + (outLeft[i] * wet);
        outRight[i] = (dryRight[i] * dry) + (outRight[i] * wet);
    }
}

} // namespace adx::plugin
