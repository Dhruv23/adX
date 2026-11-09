// `midiout`: a channel whose notes leave adX as MIDI (phase_11.5.md §4.6) - an FM-1 part,
// or any external instrument. It makes no sound of its own; the device's audio comes back
// through an audio input.
//
// Its notes are queued on the output's ring, stamped with the stream frame they belong
// to, exactly as the clock sender's pulses are, and MidiDevices' sender thread sends each
// when that frame is heard (MidiOutput.h). A note it started is always ended: a release
// (stop, a loop wrap, the channel replaced or removed) sends the note-offs.
//
// Parameters: `port` (the output's slot, -1 for none, set by the app from the port's
// name) and `channel` (1..16). In an offline render no output is published, so the
// channel is silent there, as the device would be.
#pragma once

#include <array>
#include <cstdint>

#include "engine/graph/nodes/ChannelNode.h"
#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {

enum class MidiOutParam : std::uint32_t { Port, Channel, Count };

// NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table.
inline constexpr auto kMidiOutParams = std::to_array<project::ParamDescriptor>({
    {"port", -1.0F, 7.0F, -1.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
    {"channel", 1.0F, 16.0F, 1.0F, project::Unit::Count, project::ScaleKind::Stepped,
     project::RateClass::Block, project::CurvePart::None},
});
// NOLINTEND(modernize-use-designated-initializers)

class MidiOutInstrument final : public graph::ChannelNode {
public:
    using ChannelNode::ChannelNode;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "midiout";
    }
    [[nodiscard]] std::uint32_t soundingVoices() noexcept override {
        return m_heldCount;
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
        return false;
    }

private:
    struct Held {
        std::uint32_t noteId{0};
        std::uint32_t instance{0};
        std::uint32_t source{0};
        std::int64_t endTick{0};
        std::uint8_t key{0};
        std::uint8_t channel{0};
        std::int8_t port{-1};
    };
    static constexpr std::size_t kMaxHeld = 128;

    static void send(std::int8_t port, std::uint64_t frame, std::uint8_t status, std::uint8_t data1,
                     std::uint8_t data2) noexcept;
    void release(std::size_t index, std::uint64_t frame) noexcept;

    std::array<Held, kMaxHeld> m_held{};
    std::uint32_t m_heldCount{0};
};

} // namespace adx::instruments
