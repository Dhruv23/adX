// The instrument base: ChannelNode plus per-voice state of the instrument's own type.
//
// graph::ChannelNode already owns everything voice-related that is the same for every
// instrument - dispatch, identity, stealing, glides (phase_4.md §4.2). What it cannot
// own is an instrument's per-voice DSP state, which is a different struct for every
// instrument and far more than the eight floats a Voice carries inline. This adds one
// parallel array of `State`, sized to the pool's storage and allocated in prepare(),
// indexed by the voice's slot (P3-6).
//
// It also gives every instrument the same control grid: control() is true on the
// frames where a voice's own age is a multiple of project::kControlFrames, which is
// where an instrument reads its block-rate parameters and recomputes coefficients.
// Keyed to the voice's age, never to a block boundary, so the output is the same at
// any block size.
#pragma once

#include <cstdint>
#include <type_traits>

#include "engine/graph/nodes/ChannelNode.h"
#include "engine/project/ParamDescriptor.h"
#include "engine/rt/OwnedArray.h"

namespace adx::instruments {

template<class State> class Instrument : public graph::ChannelNode {
    static_assert(std::is_trivially_destructible_v<State>,
                  "per-voice state lives in an OwnedArray: plain data only");

public:
    using ChannelNode::ChannelNode;

    void prepare(const graph::PrepareInfo& info) override {
        ChannelNode::prepare(info);
        m_states.allocate(voiceSlots());
        m_sampleRate = info.sampleRate;
        prepareInstrument(info);
    }

protected:
    [[nodiscard]] State& stateOf(const graph::Voice& voice) noexcept {
        return m_states.view()[voiceIndex(voice)];
    }

    /// True when frame `frame` of this segment is on the voice's control grid.
    [[nodiscard]] static bool control(const graph::Voice& voice, std::uint32_t frame) noexcept {
        return ((voice.age + frame) % project::kControlFrames) == 0;
    }

    [[nodiscard]] std::uint32_t sampleRate() const noexcept {
        return m_sampleRate;
    }

    /// Main thread: the instrument's own allocations. The voice state array exists.
    virtual void prepareInstrument(const graph::PrepareInfo& /*info*/) {}

private:
    rt::OwnedArray<State> m_states;
    std::uint32_t m_sampleRate{48000};
};

} // namespace adx::instruments
