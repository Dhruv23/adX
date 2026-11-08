// adx-thread: realtime
//
// A hosted plugin as a channel's instrument (phase_9.md §4.5). The plugin has voices of
// its own, so this renders whole blocks (ChannelNode::renderWhole): the block's note
// events go to the sandbox as note events, at their offsets, and the plugin's stereo
// output comes back. The channel's volume, pan and mute apply as for any instrument.
//
// When the plugin does not answer, the channel is silent for that block (§4.4).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "engine/graph/DeclaresParams.h"
#include "engine/graph/nodes/ChannelNode.h"
#include "engine/plugin/PluginCore.h"
#include "engine/rt/OwnedArray.h"

namespace adx::plugin {

class PluginInstrument final : public graph::ChannelNode, public graph::DeclaresParams {
public:
    /// Main thread.
    PluginInstrument(std::uint32_t channelId, std::string_view format,
                     const project::PluginRef& ref);

    void prepare(const graph::PrepareInfo& info) override;
    [[nodiscard]] std::uint32_t latencySamples() const noexcept override {
        return m_core.latency();
    }
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
    bool renderWhole(const graph::ProcessContext& context, std::span<float> left,
                     std::span<float> right, bool releaseAll) noexcept override;
    [[nodiscard]] bool wholeSounding() const noexcept override {
        return m_tailLeft > 0;
    }
    void startVoice(graph::Voice& /*voice*/, const graph::BlockEvent& /*event*/,
                    const graph::VoiceRender& /*render*/) noexcept override {}
    bool renderVoice(graph::Voice& /*voice*/, std::span<float> /*left*/, std::span<float> /*right*/,
                     const graph::VoiceRender& /*render*/) noexcept override {
        return false;
    }

private:
    /// A note the plugin is sounding: which key its note-off must name.
    struct Held {
        std::uint32_t noteId;
        std::uint32_t instance;
        std::int64_t endTick;
        std::uint8_t key;
        bool active;
        /// The note's own pitch offset (slides, pitch curves) in cents, where a glide is
        /// headed, and the frames left to get there (P9-7).
        float cents;
        float target;
        std::uint32_t remaining;
    };
    static constexpr std::size_t kMaxHeld = 128;
    /// How long a departing plugin is rendered after its release, in seconds.
    static constexpr double kTailSeconds = 2.0;

    void releaseHeld(std::uint32_t offset, bool pastLoopEnd, std::int64_t loopEnd) noexcept;
    void push(const bridge::WireEvent& event) noexcept;
    /// A tuning event for `held`: its own offset plus the channel's.
    void tune(const Held& held, std::uint32_t offset) noexcept;
    /// Glides in flight, as tuning steps across this block.
    void stepGlides(std::uint32_t frames) noexcept;
    /// The events in offset order (insertion sort: no allocation, and nearly sorted).
    void sortEvents() noexcept;

    PluginCore m_core;
    std::array<Held, kMaxHeld> m_held{};
    rt::OwnedArray<bridge::WireEvent> m_events;
    std::uint32_t m_eventCount{0};
    std::uint32_t m_sampleRate{48000};
    std::uint64_t m_tailLeft{0};
    /// The channel's pitch offset last sent, in cents.
    float m_channelCents{0.0F};
};

} // namespace adx::plugin
