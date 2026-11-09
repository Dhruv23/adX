// `dx7`: the FM-1's engine as a built-in instrument (phase_11.5.md §4.2).
//
// msfa renders 64-sample fixed-point blocks for all of its notes at once, as the device
// does, so this is a whole-block instrument (renderWhole) over Dx7Engine rather than a
// per-voice one. Three rules keep it block-size independent, and an offline render
// equal to a live one:
// - the 64-sample grid starts at the note-on that wakes an idle instrument, never at a
//   buffer boundary, and the LFO starts from upstream's initial phase there;
// - an event applies at the first block that starts at or after it (at most 63
//   samples late, the device's own granularity), whatever buffer it arrived in;
// - parameters are read at each block's first frame.
//
// FM-1-rate mode renders at 44 118 Hz and reads the blocks back at the engine's rate
// through a 4-point Hermite interpolator, so aliasing and envelope timing are the
// device's. Events then apply three internal samples later than the rule above, the
// interpolator's look-ahead, so a later buffer's event can never land in a block that
// has already been rendered.
#pragma once

#include <array>
#include <cstdint>

#include "engine/graph/nodes/ChannelNode.h"
#include "engine/instruments/dx7/Dx7Engine.h"
#include "engine/instruments/dx7/Dx7Params.h"

namespace adx::dx7 {

inline constexpr double kFm1Rate = 44118.0;

class Dx7Instrument final : public graph::ChannelNode {
public:
    using ChannelNode::ChannelNode;

    void prepare(const graph::PrepareInfo& info) override;
    void reset() noexcept override;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "dx7";
    }

    [[nodiscard]] std::uint32_t soundingVoices() noexcept override {
        return static_cast<std::uint32_t>(m_engines[m_mode].liveVoices());
    }

    /// For tests: the engine playing now.
    [[nodiscard]] const Dx7Engine& engine() const noexcept {
        return m_engines[m_mode];
    }

protected:
    void startVoice(graph::Voice& /*voice*/, const graph::BlockEvent& /*event*/,
                    const graph::VoiceRender& /*render*/) noexcept override {}
    bool renderVoice(graph::Voice& /*voice*/, std::span<float> /*left*/, std::span<float> /*right*/,
                     const graph::VoiceRender& /*render*/) noexcept override {
        return false;
    }
    bool renderWhole(const graph::ProcessContext& context, std::span<float> left,
                     std::span<float> right, bool releaseAll) noexcept override;
    [[nodiscard]] bool wholeSounding() const noexcept override {
        return m_awake;
    }

private:
    struct Pending {
        std::int64_t at;
        graph::BlockEvent event;
    };
    static constexpr std::size_t kPendingCapacity = 512;
    static constexpr std::size_t kRing = 128;

    void readParams(const graph::ProcessContext& context, std::uint32_t frame) noexcept;
    void apply(const graph::BlockEvent& event) noexcept;
    void renderBlock(const graph::ProcessContext& context, std::uint32_t frame) noexcept;
    void wake(std::int64_t at) noexcept;
    [[nodiscard]] float sampleAt(std::int64_t hostFrame) noexcept;

    std::array<Dx7Engine, 2> m_engines{};
    std::size_t m_mode{0};
    double m_hostRate{48000.0};
    double m_ratio{1.0};
    std::array<Pending, kPendingCapacity> m_pending{};
    std::size_t m_pendingCount{0};
    std::array<float, kRing> m_ring{};
    std::array<std::int32_t, 64> m_block{};
    std::int64_t m_now{0};
    std::int64_t m_wakeAt{0};
    std::int64_t m_produced{0};
    bool m_awake{false};
    bool m_sustain{false};
    float m_gain{1.0F};
    float m_panLeft{1.0F};
    float m_panRight{1.0F};
    std::uint64_t m_droppedEvents{0};
};

} // namespace adx::dx7
