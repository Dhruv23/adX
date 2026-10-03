#include "engine/graph/nodes/InsertNode.h"

#include <algorithm>

#include "engine/graph/nodes/Gain.h"

namespace adx::graph {

void InsertNode::prepare(const PrepareInfo& /*info*/) {}

void InsertNode::reset() noexcept {}

PortSpec InsertNode::ports() const noexcept {
    return PortSpec{.inputs = 1, .outputs = 2, .acceptsEvents = false};
}

void InsertNode::process(ProcessContext& context) noexcept {
    const std::span<const float> inLeft = context.inputs[0];
    const std::span<const float> inRight = context.inputs[1];
    const std::span<float> postLeft = context.outputs[(kPortPost * kPortChannels) + 0];
    const std::span<float> postRight = context.outputs[(kPortPost * kPortChannels) + 1];
    const std::span<float> preLeft = context.outputs[(kPortPre * kPortChannels) + 0];
    const std::span<float> preRight = context.outputs[(kPortPre * kPortChannels) + 1];

    std::ranges::copy(inLeft, preLeft.begin());
    std::ranges::copy(inRight, preRight.begin());
    std::ranges::copy(inLeft, postLeft.begin());
    std::ranges::copy(inRight, postRight.begin());

    const auto param = [&context](InsertParam which) {
        return context.params[static_cast<std::uint32_t>(which)];
    };
    constexpr auto kGain = static_cast<std::uint32_t>(InsertParam::Gain);
    constexpr auto kPan = static_cast<std::uint32_t>(InsertParam::Pan);
    constexpr auto kWidth = static_cast<std::uint32_t>(InsertParam::Width);

    // Automated: per frame, so a fader ride is a ramp and not a staircase at block
    // edges (P3-3, for inserts as Phase 4 did it for channels).
    if (paramMoves(context, kGain) || paramMoves(context, kPan) || paramMoves(context, kWidth)) {
        const float fixed = param(InsertParam::Audible) * param(InsertParam::Polarity);
        for (std::uint32_t i = 0; i < context.frames; ++i) {
            const float width = paramAt(context, kWidth, i);
            if (width != 1.0F) {
                const float mid = (postLeft[i] + postRight[i]) * 0.5F;
                const float side = (postLeft[i] - postRight[i]) * 0.5F * width;
                postLeft[i] = mid + side;
                postRight[i] = mid - side;
            }
            const PanGains sides = balance(paramAt(context, kPan, i));
            const float gain = paramAt(context, kGain, i) * fixed;
            postLeft[i] *= gain * sides.left;
            postRight[i] *= gain * sides.right;
        }
        return;
    }

    // Width only when it is not unity. Mid/side and back is not bit-exact at w = 1 -
    // (L+R)/2 + (L-R)/2 need not round to L - and a strip nobody touched must pass its
    // input through untouched, or no hand-computed routing test could ever pass.
    const float width = param(InsertParam::Width);
    if (width != 1.0F) {
        for (std::size_t i = 0; i < postLeft.size(); ++i) {
            const float mid = (postLeft[i] + postRight[i]) * 0.5F;
            const float side = (postLeft[i] - postRight[i]) * 0.5F * width;
            postLeft[i] = mid + side;
            postRight[i] = mid - side;
        }
    }

    const float gain =
        param(InsertParam::Gain) * param(InsertParam::Audible) * param(InsertParam::Polarity);
    applyGainPan(postLeft, postRight, gain, param(InsertParam::Pan));
}

} // namespace adx::graph
