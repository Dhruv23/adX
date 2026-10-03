#include "tests/cpp/effects/EffectHarness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>

#include "engine/dsp/Noise.h"
#include "engine/effects/Factory.h"
#include "engine/project/Mixer.h"
#include "engine/project/TypeCatalog.h"
#include "engine/rt/BlockArena.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"
#include "engine/transport/TimeSource.h"

namespace adx::tests {

std::vector<float> slotParams(std::string_view type, float mix, bool bypass) {
    std::vector<float> params{mix, bypass ? 1.0F : 0.0F};
    if (const project::TypeInfo* info = project::findEffectType(type)) {
        for (const project::ParamDescriptor& descriptor : info->params) {
            params.push_back(descriptor.defaultValue);
        }
    }
    return params;
}

std::uint32_t slotParamIndex(std::string_view type, std::string_view name) {
    const project::TypeInfo* info = project::findEffectType(type);
    if (info == nullptr) {
        return project::kNoParam;
    }
    const std::uint32_t index = project::paramIndexOf(*info, name);
    return index == project::kNoParam ? index : index + graph::kSlotParamCount;
}

StereoSignal sine(double frequency, float amplitude, std::size_t frames) {
    StereoSignal signal{.left = std::vector<float>(frames), .right = std::vector<float>(frames)};
    for (std::size_t i = 0; i < frames; ++i) {
        const double phase =
            2.0 * std::numbers::pi * frequency * static_cast<double>(i) / kTestRate;
        signal.left[i] = amplitude * static_cast<float>(std::sin(phase));
        signal.right[i] = signal.left[i];
    }
    return signal;
}

StereoSignal noise(std::uint32_t seed, float amplitude, std::size_t frames) {
    StereoSignal signal{.left = std::vector<float>(frames), .right = std::vector<float>(frames)};
    dsp::WhiteNoise left(seed);
    dsp::WhiteNoise right(seed ^ 0x5bd1e995U);
    for (std::size_t i = 0; i < frames; ++i) {
        signal.left[i] = amplitude * left.next();
        signal.right[i] = amplitude * right.next();
    }
    return signal;
}

StereoSignal impulse(std::size_t at, float amplitude, std::size_t frames) {
    StereoSignal signal{.left = std::vector<float>(frames), .right = std::vector<float>(frames)};
    signal.left[at] = amplitude;
    signal.right[at] = amplitude;
    return signal;
}

EffectRun runEffect(graph::SlotNode& node, const StereoSignal& in, std::vector<float> params,
                    std::uint32_t block, const ParamSchedule& schedule, const StereoSignal* side) {
    const std::size_t frames = in.size();
    EffectRun run;
    run.out.left.assign(frames, 0.0F);
    run.out.right.assign(frames, 0.0F);

    std::vector<std::byte> arenaStorage(1U << 20U);
    rt::BlockArena arena{arenaStorage.data(), arenaStorage.size()};
    const transport::TimeSource time;
    const bool sidechain = node.ports().inputs >= 2;
    const std::vector<float> silence(frames, 0.0F);
    const std::span<const float> sideLeft =
        side != nullptr ? std::span<const float>{side->left} : std::span<const float>{silence};
    const std::span<const float> sideRight =
        side != nullptr ? std::span<const float>{side->right} : std::span<const float>{silence};

    rt::ViolationLog& log = rt::ViolationLog::instance();
    log.reset();
    for (std::size_t start = 0; start < frames; start += block) {
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(block, frames - start));
        if (schedule) {
            schedule(start, params);
        }
        const std::array<std::span<const float>, 4> inputs{
            std::span<const float>{in.left}.subspan(start, count),
            std::span<const float>{in.right}.subspan(start, count), sideLeft.subspan(start, count),
            sideRight.subspan(start, count)};
        const std::array<std::span<float>, 2> outputs{
            std::span<float>{run.out.left}.subspan(start, count),
            std::span<float>{run.out.right}.subspan(start, count)};
        arena.reset();
        graph::ProcessContext context{
            .time = time,
            .outputs = outputs,
            .inputs = std::span<const std::span<const float>>{inputs}.first(sidechain ? 4 : 2),
            .frames = count,
            .sampleRate = kTestRate,
            .events = {},
            .params = params,
            .automation = {},
            .arena = arena};
        {
            const rt::ScopedRtSection section;
            node.process(context);
        }
    }
    run.violations =
        log.count(rt::ViolationKind::Allocation) + log.count(rt::ViolationKind::Deallocation);
    return run;
}

std::shared_ptr<graph::SlotNode> preparedEffect(std::string_view type) {
    // Through a Slot, so a lookahead effect gets its type's default lookahead - the
    // structural configuration makeEffect(type) alone leaves at zero.
    project::Slot slot;
    slot.type = std::string(type);
    std::shared_ptr<graph::SlotNode> node = effects::makeEffect(slot);
    node->prepare(graph::PrepareInfo{.sampleRate = kTestRate, .maxBlockFrames = 2048});
    return node;
}

} // namespace adx::tests
