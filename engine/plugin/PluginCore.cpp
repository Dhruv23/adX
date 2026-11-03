// adx-thread: realtime
#include "engine/plugin/PluginCore.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/core/Time.h"
#include "engine/project/ParamDescriptor.h"

namespace adx::plugin {

namespace {

constexpr std::uint32_t kNoId = 0xFFFFFFFFU;

bool finite(std::span<float* const> outputs, std::uint32_t frames) noexcept {
    for (float* out : outputs) {
        for (std::uint32_t i = 0; i < frames; ++i) {
            if (!std::isfinite(out[i])) {
                return false;
            }
        }
    }
    return true;
}

} // namespace

std::uint32_t PluginCore::liveLatency() const noexcept {
    return m_link.shared == nullptr
               ? 0
               : m_link.shared->control.latency.load(std::memory_order_relaxed);
}

void PluginCore::collectParams(std::span<const float> params,
                               std::span<const float* const> automation,
                               std::uint32_t frames) noexcept {
    m_changeCount = 0;
    const std::span<bridge::WireParam> changes = m_changes.view();
    const std::span<float> sent = m_sent.view();
    const std::span<const std::uint32_t> ids = m_ids.view();
    const std::size_t count = std::min({params.size(), sent.size(), ids.size()});
    const auto push = [&](std::uint32_t offset, std::size_t i, float value) {
        if (std::isnan(value) || ids[i] == kNoId || value == sent[i] ||
            m_changeCount >= changes.size()) {
            return;
        }
        changes[m_changeCount++] =
            bridge::WireParam{.offset = offset, .id = ids[i], .value = static_cast<double>(value)};
        sent[i] = value;
    };
    // Block values first, at offset 0; then each moving parameter on the control grid,
    // so a sweep reaches the plugin as a sweep and not as one step a block.
    for (std::size_t i = 0; i < count; ++i) {
        const bool moves = i < automation.size() && automation[i] != nullptr;
        push(0, i, moves ? automation[i][0] : params[i]);
    }
    for (std::uint32_t frame = project::kControlFrames; frame < frames;
         frame += project::kControlFrames) {
        for (std::size_t i = 0; i < count && i < automation.size(); ++i) {
            if (automation[i] != nullptr) {
                push(frame, i, automation[i][frame]);
            }
        }
    }
    // Sorted by offset for the plugin, which the two passes above already are.
}

CoreResult PluginCore::process(std::span<const float* const> inputs,
                               std::span<float* const> outputs, std::uint32_t frames,
                               std::span<const float> params,
                               std::span<const float* const> automation,
                               std::span<const bridge::WireEvent> events,
                               const transport::TimeSource& time) noexcept {
    if (m_link.health == nullptr) {
        return CoreResult::Absent;
    }
    bridge::LinkHealth& health = *m_link.health;
    // A (re)loaded instance has none of our values: send them all again.
    const std::uint32_t generation = health.generation.load(std::memory_order_acquire);
    if (generation != m_generation) {
        m_generation = generation;
        std::ranges::fill(m_sent.view(), std::numeric_limits<float>::quiet_NaN());
    }
    const bridge::LinkState state = health.state.load(std::memory_order_acquire);
    if (state == bridge::LinkState::Muted) {
        for (float* out : outputs) {
            std::fill_n(out, frames, 0.0F);
        }
        return CoreResult::Silent;
    }
    if (state != bridge::LinkState::Running) {
        return CoreResult::Absent;
    }
    collectParams(params, automation, frames);

    const core::Ticks ticks = time.positionTicks();
    bridge::BlockRequest request;
    request.inputs = inputs;
    request.frames = frames;
    request.events = events;
    request.params = std::span<const bridge::WireParam>(m_changes.view().data(), m_changeCount);
    request.transport.tempo = time.tempo().valid() ? time.tempo().bpmAt(ticks) : 120.0;
    request.transport.beats = static_cast<double>(ticks.value) / static_cast<double>(core::kPpq);
    request.transport.samples = time.positionSamples();
    request.transport.flags = time.rolling() ? bridge::kPlaying : 0U;
    request.transport.numerator = 4;
    request.transport.denominator = 4;
    request.deadlineMicros = bridge::deadlineMicros(m_sampleRate, frames, m_offline);

    std::uint32_t outCount = 0;
    const bridge::BlockResult result =
        bridge::runBlock(m_link, request, outputs, m_outParams.view(), outCount);
    if (result == bridge::BlockResult::NotRunning) {
        return CoreResult::Absent;
    }
    if (m_late.block(frames, result == bridge::BlockResult::Late) && !m_offline) {
        health.state.store(bridge::LinkState::LateBypassed, std::memory_order_release);
        health.stateChanged.store(true, std::memory_order_release);
    }
    if (result == bridge::BlockResult::Late) {
        return CoreResult::Silent;
    }
    if (!finite(outputs, frames)) {
        for (float* out : outputs) {
            std::fill_n(out, frames, 0.0F);
        }
        health.nanBlocks.fetch_add(1, std::memory_order_relaxed);
        health.state.store(bridge::LinkState::Muted, std::memory_order_release);
        health.stateChanged.store(true, std::memory_order_release);
        return CoreResult::Silent;
    }
    // The plugin's own changes (its editor): to the main thread, and remembered as sent
    // so they are not echoed straight back.
    const std::span<const bridge::WireParam> echoes = m_outParams.view().first(outCount);
    const std::span<const std::uint32_t> ids = m_ids.view();
    const std::span<float> sent = m_sent.view();
    for (const bridge::WireParam& change : echoes) {
        for (std::size_t i = 0; i < ids.size(); ++i) {
            if (ids[i] == change.id) {
                sent[i] = static_cast<float>(change.value);
                static_cast<void>(
                    m_echoes.tryPush(ParamEcho{.index = static_cast<std::uint32_t>(i),
                                               .value = static_cast<float>(change.value)}));
                break;
            }
        }
    }
    return CoreResult::Played;
}

} // namespace adx::plugin
