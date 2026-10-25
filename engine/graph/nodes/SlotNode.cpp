#include "engine/graph/nodes/SlotNode.h"

#include <algorithm>
#include <array>

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
    m_last.capture(context);
    processSlot(context);
    if (m_handover.from != nullptr) {
        crossfadeFromOld(context);
    }
}

void SlotNode::crossfadeFromOld(ProcessContext& context) noexcept {
    SlotNode& old = *m_handover.from;
    const std::uint32_t frames = context.frames;
    const std::span<float> oldLeft = context.arena.allocate<float>(frames);
    const std::span<float> oldRight = context.arena.allocate<float>(frames);
    if (!old.m_last.valid() || m_handover.total == 0 || oldLeft.size() != frames ||
        oldRight.size() != frames) {
        m_handover.finish();
        return;
    }
    const std::array<std::span<float>, 2> outputs{oldLeft, oldRight};
    ProcessContext oldContext{.time = context.time,
                              .outputs = outputs,
                              .inputs = context.inputs,
                              .frames = frames,
                              .sampleRate = context.sampleRate,
                              .events = {},
                              .params = old.m_last.view(),
                              .automation = {},
                              .arena = context.arena,
                              .tuningCents = context.tuningCents};
    // Its own block only: a slot handing over does not continue a crossfade of its own,
    // which keeps this from recursing down a chain of replacements. (A second type
    // change within the first's 20 ms cuts the first's tail.)
    old.processSlot(oldContext);

    // Linear crossfade: the old instance's gain falls from 1 to 0 over the fade, the new
    // one's rises, so their sum never jumps.
    const std::span<float> left = context.outputs[0];
    const std::span<float> right = context.outputs[1];
    const auto total = static_cast<float>(m_handover.total);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint32_t remaining = m_handover.remaining > i ? m_handover.remaining - i : 0;
        const float oldGain = static_cast<float>(remaining) / total;
        const float newGain = 1.0F - oldGain;
        left[i] = (left[i] * newGain) + (oldLeft[i] * oldGain);
        right[i] = (right[i] * newGain) + (oldRight[i] * oldGain);
    }
    m_handover.remaining -= std::min(frames, m_handover.remaining);
    if (m_handover.remaining == 0) {
        m_handover.finish();
    }
}

void SlotNode::processSlot(ProcessContext& context) noexcept {
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
