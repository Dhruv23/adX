// An instrument instance and its voice pool: the node a Channel becomes.
//
// This is the part of an instrument that is the same for every instrument - event
// dispatch, voice identity, stealing and the steal fade, pitch glides, portamento, and
// the channel's own volume, pan and mute. A concrete instrument derives from it and
// implements startVoice and renderVoice: one voice's DSP, never the pool
// (phase_3.md §9, phase_4.md §4.2 - this class is the `Instrument` base that section
// sketches; engine/instruments/Instrument.h adds per-voice state on top).
//
// Pitch is the base's business, once, so that slides, pitch curves and portamento
// work on every instrument for free (phase_4.md §4.2). A voice is handed a per-frame
// span of cents - channel pitch, plus portamento, plus its slide and pitch-curve
// glides - and turns it into a frequency. Vibrato is the instrument's own and is
// summed into that span by the instrument, in cents.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "engine/graph/Node.h"
#include "engine/graph/VoicePool.h"
#include "engine/project/VoiceStealMode.h"
#include "engine/rt/OwnedArray.h"

namespace adx::graph {

/// A channel's parameters, in the order they sit in its parameter slice. The
/// instrument's own follow, in the order its descriptor table names them.
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

/// What a voice is rendered against: one segment of one process() call.
struct VoiceRender {
    std::uint32_t sampleRate{48000};
    /// The voice's pitch offset from its MIDI note, in cents, for each frame of the
    /// segment: channel pitch + portamento + slide + pitch curve.
    std::span<const float> pitchCents;
    /// The instrument's parameters (after the channel's own), at the start of the call.
    std::span<const float> params;
    /// Per-frame automation of those parameters, indexed like `params`; see paramAt.
    std::span<const float* const> automation;
    /// Where this segment starts within the process() call, which is what the
    /// automation spans are indexed from.
    std::uint32_t frameOffset{0};

    /// Instrument parameter `index` at frame `frame` of this segment.
    [[nodiscard]] float paramAt(std::uint32_t index, std::uint32_t frame) const noexcept {
        if (index < automation.size() && automation[index] != nullptr) {
            return automation[index][frameOffset + frame];
        }
        return index < params.size() ? params[index] : 0.0F;
    }
    /// The parameter's value at the segment's first frame.
    [[nodiscard]] float param(std::uint32_t index) const noexcept {
        return paramAt(index, 0);
    }
};

class ChannelNode : public Node {
public:
    ChannelNode(std::uint32_t channelId, std::uint16_t maxPolyphony,
                project::VoiceStealMode stealMode) noexcept;

    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& context) noexcept final;
    void reset() noexcept override;
    [[nodiscard]] PortSpec ports() const noexcept override;

    /// The instrument type this node implements, as the catalog names it. What the
    /// node store compares to decide whether a changed channel can keep its node.
    [[nodiscard]] virtual std::string_view typeName() const noexcept = 0;

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
    /// A voice has just been allocated for `event`. Initialise its state. `render`
    /// carries the parameters at the note's frame; its pitch span is empty.
    virtual void startVoice(Voice& voice, const BlockEvent& event,
                            const VoiceRender& render) noexcept = 0;

    /// Writes (overwrites) `left` and `right` with this voice's next samples and keeps
    /// voice.level current. Runs its release when voice.phase is Released. Returns
    /// false once the voice has finished and can be freed.
    virtual bool renderVoice(Voice& voice, std::span<float> left, std::span<float> right,
                             const VoiceRender& render) noexcept = 0;

    /// Channel-level portamento time, from the instrument's parameters. Zero (the
    /// default) is off. Applies only when a note starts while another is sounding -
    /// a legato line - and an explicit slide on the note overrides it.
    [[nodiscard]] virtual float portamentoSeconds(std::span<const float> params) const noexcept;

    /// The slot of `voice` in the pool's storage: the index an instrument's parallel
    /// per-voice state array is read at (P3-6).
    [[nodiscard]] std::size_t voiceIndex(const Voice& voice) const noexcept;

    /// Storage size every per-voice array must have.
    [[nodiscard]] std::size_t voiceSlots() const noexcept {
        return VoicePool::storageFor(m_maxPolyphony);
    }

private:
    void apply(const BlockEvent& event, const VoiceRender& render) noexcept;
    void renderSegment(std::span<float> left, std::span<float> right, std::span<float> scratchLeft,
                       std::span<float> scratchRight, std::span<float> cents,
                       const ProcessContext& context, VoiceRender& render) noexcept;
    static void applyChannelStrip(const ProcessContext& context) noexcept;

    std::uint32_t m_channelId;
    std::uint16_t m_maxPolyphony;
    project::VoiceStealMode m_stealMode;
    rt::OwnedArray<Voice> m_voiceStorage;
    VoicePool m_pool;

    /// The last note-on's pitch, for portamento. Node state, so a glide from the
    /// previous note survives across blocks and is identical offline and realtime.
    std::uint8_t m_lastPitch{0};
    bool m_hasLastPitch{false};
};

/// The node for a channel whose instrument type this build does not know: it keeps
/// the channel's place in the graph and its event track, and plays nothing. A newer
/// adX's instrument loads in an older one as silence, not as an error (FINAL_PLAN §6
/// rule 4).
class SilentChannelNode final : public ChannelNode {
public:
    using ChannelNode::ChannelNode;
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return {};
    }

protected:
    void startVoice(Voice& /*voice*/, const BlockEvent& /*event*/,
                    const VoiceRender& /*render*/) noexcept override {}
    bool renderVoice(Voice& voice, std::span<float> left, std::span<float> right,
                     const VoiceRender& /*render*/) noexcept override;
};

} // namespace adx::graph
