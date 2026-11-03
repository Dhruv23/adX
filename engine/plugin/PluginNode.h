// adx-thread: realtime
//
// A hosted plugin in an effect slot (phase_9.md §4.5): an ordinary slot node whose wet
// signal comes back from the sandbox. Its latency is the plugin's, so Phase 3's PDC
// compensates it like any other node's; its dry path is delayed by the same amount
// here, so a mix below 1 does not comb.
//
// What the slot plays when the plugin does not answer (§4.4):
//   not running (loading, crashed, bypassed for lateness)  the dry signal
//   late, or muted for NaN                                  silence for that block
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "engine/graph/DeclaresParams.h"
#include "engine/graph/nodes/SlotNode.h"
#include "engine/plugin/PluginCore.h"
#include "engine/rt/OwnedArray.h"

namespace adx::plugin {

class PluginEffect final : public graph::SlotNode, public graph::DeclaresParams {
public:
    /// Main thread.
    PluginEffect(std::string_view format, const project::PluginRef& ref);

    void prepare(const graph::PrepareInfo& info) override;
    void reset() noexcept override;
    [[nodiscard]] std::uint32_t latencySamples() const noexcept override;
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return m_core.formatName();
    }

    [[nodiscard]] std::uint32_t declaredParamCount() const noexcept override {
        return m_core.paramCount();
    }
    [[nodiscard]] std::string_view declaredParamName(std::uint32_t index) const noexcept override {
        return m_core.paramKey(index);
    }

    [[nodiscard]] PluginCore& core() noexcept {
        return m_core;
    }
    [[nodiscard]] const PluginCore& core() const noexcept {
        return m_core;
    }

protected:
    void processSlot(graph::ProcessContext& context) noexcept override;

private:
    /// Longest latency the dry path can follow; a plugin reporting more is clamped.
    static constexpr std::uint32_t kMaxDryDelay = 1U << 16U;

    [[nodiscard]] std::uint32_t latencySamplesLive() const noexcept {
        const std::uint32_t live = m_core.liveLatency();
        return live < kMaxDryDelay ? live : kMaxDryDelay - 1;
    }
    void delayDry(std::span<const float> inLeft, std::span<const float> inRight,
                  std::span<float> dryLeft, std::span<float> dryRight,
                  std::uint32_t delay) noexcept;

    PluginCore m_core;
    rt::OwnedArray<float> m_ringLeft;
    rt::OwnedArray<float> m_ringRight;
    std::size_t m_write{0};
};

} // namespace adx::plugin
