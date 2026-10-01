// An instrument instance and its voice pool: the node a Channel becomes.
//
// This is the part of an instrument that is the same for every instrument - event
// dispatch, voice identity, stealing and the steal fade, and the channel's own volume,
// pan and mute. A concrete instrument derives from it and implements two functions,
// startVoice and renderVoice, which is the whole of what Phase 4 has to write per
// synth (phase_3.md §9). TestToneNode is the reference implementation.
#pragma once

#include <cstdint>
#include <span>

#include "engine/graph/Node.h"
#include "engine/graph/VoicePool.h"
#include "engine/project/VoiceStealMode.h"
#include "engine/rt/OwnedArray.h"

namespace adx::graph {

/// A channel's parameters, in the order they sit in its parameter slice.
enum class ChannelParam : std::uint32_t {
    Volume,
    Pan,
    /// 1 when audible, 0 when muted - by its own mute, or by another channel's solo.
    /// Resolved on the main thread, so the audio thread never evaluates solo logic.
    Audible,
    PitchCents,
    Count,
};

inline constexpr std::uint32_t kChannelParamCount = static_cast<std::uint32_t>(ChannelParam::Count);

class ChannelNode : public Node {
public:
    ChannelNode(std::uint32_t channelId, std::uint16_t maxPolyphony,
                project::VoiceStealMode stealMode) noexcept;

    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& context) noexcept final;
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;

    [[nodiscard]] std::uint32_t channelId() const noexcept {
        return m_channelId;
    }
    [[nodiscard]] std::uint16_t maxPolyphony() const noexcept {
        return m_maxPolyphony;
    }
    [[nodiscard]] project::VoiceStealMode stealMode() const noexcept {
        return m_stealMode;
    }
    /// For tests, and for Phase 6's voice-count readout. Not for the audio thread's
    /// callers - it is the node's own state.
    [[nodiscard]] VoicePool& pool() noexcept {
        return m_pool;
    }

protected:
    /// A voice has just been allocated for `event`. Initialise its state.
    virtual void startVoice(Voice& voice, const BlockEvent& event,
                            std::uint32_t sampleRate) noexcept = 0;

    /// Writes (overwrites) `left` and `right` with this voice's next samples and keeps
    /// voice.level current. Runs its release when voice.phase is Released. Returns
    /// false once the voice has finished and can be freed.
    virtual bool renderVoice(Voice& voice, std::span<float> left, std::span<float> right,
                             std::uint32_t sampleRate, float pitchCents) noexcept = 0;

private:
    void apply(const BlockEvent& event, std::uint32_t sampleRate) noexcept;
    void renderSegment(std::span<float> left, std::span<float> right, std::span<float> scratchLeft,
                       std::span<float> scratchRight, std::uint32_t sampleRate,
                       float pitchCents) noexcept;

    std::uint32_t m_channelId;
    std::uint16_t m_maxPolyphony;
    project::VoiceStealMode m_stealMode;
    rt::OwnedArray<Voice> m_voiceStorage;
    VoicePool m_pool;
};

} // namespace adx::graph
