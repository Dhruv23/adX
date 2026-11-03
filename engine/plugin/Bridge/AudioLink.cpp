// adx-thread: realtime
#include "engine/plugin/Bridge/AudioLink.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace adx::plugin::bridge {

namespace {

std::int64_t micros() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void silence(std::span<float* const> outputs, std::uint32_t frames) noexcept {
    for (float* out : outputs) {
        std::fill_n(out, frames, 0.0F);
    }
}

} // namespace

BlockResult runBlock(const AudioLink& link, const BlockRequest& request,
                     std::span<float* const> outputs, std::span<WireParam> outParams,
                     std::uint32_t& outParamCount) noexcept {
    outParamCount = 0;
    if (link.shared == nullptr || link.health == nullptr ||
        link.health->state.load(std::memory_order_acquire) != LinkState::Running) {
        return BlockResult::NotRunning;
    }
    SharedAudio& shared = *link.shared;
    ControlBlock& control = shared.control;
    const std::uint32_t frames = std::min(request.frames, kMaxBlockFrames);

    // The child is still on an earlier block: do not touch the region. This block is
    // late, and the waiting clock keeps running from when that block was sent.
    const std::uint32_t sent = control.request.load(std::memory_order_relaxed);
    if (control.reply.load(std::memory_order_acquire) != sent) {
        silence(outputs, frames);
        link.health->lateBlocks.fetch_add(1, std::memory_order_relaxed);
        return BlockResult::Late;
    }

    for (std::size_t c = 0; c < kMaxChannels; ++c) {
        float* into = shared.input.at(c).data();
        const float* from = c < request.inputs.size() ? request.inputs[c] : nullptr;
        if (from != nullptr) {
            std::copy_n(from, frames, into);
        } else {
            std::fill_n(into, frames, 0.0F);
        }
    }
    const auto eventCount =
        static_cast<std::uint32_t>(std::min<std::size_t>(request.events.size(), kMaxEvents));
    std::copy_n(request.events.data(), eventCount, shared.events.data());
    const auto paramCount =
        static_cast<std::uint32_t>(std::min<std::size_t>(request.params.size(), kMaxParamChanges));
    std::copy_n(request.params.data(), paramCount, shared.params.data());
    control.frames = frames;
    control.eventCount = eventCount;
    control.paramCount = paramCount;
    control.transport = request.transport;

    const std::uint32_t next = sent + 1;
    link.health->waitingSince.store(micros(), std::memory_order_relaxed);
    control.request.store(next, std::memory_order_release);
    signalHandle(link.go);

    // Bounded by the deadline. Waited in slices so that a child the watchdog has
    // found dead (or a restart) ends a long offline wait at once rather than at its
    // end; an event left signalled by an earlier, late reply just costs a loop.
    constexpr std::int64_t kSliceMicros = 20'000;
    const std::int64_t start = micros();
    while (control.reply.load(std::memory_order_acquire) != next) {
        const std::int64_t spent = micros() - start;
        if (std::cmp_greater_equal(spent, request.deadlineMicros) ||
            link.health->state.load(std::memory_order_acquire) != LinkState::Running) {
            silence(outputs, frames);
            link.health->lateBlocks.fetch_add(1, std::memory_order_relaxed);
            return BlockResult::Late;
        }
        const std::int64_t slice =
            std::min<std::int64_t>(request.deadlineMicros - spent, kSliceMicros);
        static_cast<void>(waitHandle(link.done, link.timer, static_cast<std::uint32_t>(slice)));
    }
    link.health->waitingSince.store(0, std::memory_order_relaxed);
    link.health->blocks.fetch_add(1, std::memory_order_relaxed);

    for (std::size_t c = 0; c < outputs.size(); ++c) {
        const float* from = shared.output.at(std::min<std::size_t>(c, kMaxChannels - 1)).data();
        std::copy_n(from, frames, outputs[c]);
    }
    outParamCount = std::min<std::uint32_t>(
        {control.outParamCount, kMaxOutParams, static_cast<std::uint32_t>(outParams.size())});
    std::copy_n(shared.outParams.data(), outParamCount, outParams.data());
    return BlockResult::Done;
}

} // namespace adx::plugin::bridge
