// The Sampler: zones, loop modes, velocity layers and round robin over pool samples
// (phase_4.md §4.5).
//
// Zones are baked into the node when it is built, already resolved to sample handles,
// and never change after prepare (ZoneSet.h).
//
// Playback: position in the sample's own frames, advanced by (sample rate / output
// rate) x 2^(semitones from the root + cents / 1200) per frame. When that ratio is
// exactly 1 - a drum at its root key, at the project's rate - the sample is copied,
// bit for bit, with no interpolation (sampler_root_key_fast_path); otherwise it is read
// through the 8-point windowed sinc, or linearly when the `interpolation` knob says so.
//
// One voice plays one zone. Velocity layers are zones with disjoint velocity ranges;
// zones in a round-robin group take turns from a counter that the node owns and resets
// whenever playback is reset, so an offline render and a realtime one from the same
// start choose the same zones (sampler_roundrobin_deterministic). Overlapping zones
// outside a group do not stack: the first in file order plays.
//
// A zone with an `end` stops there, with a short fade so a slice cut mid-waveform does
// not click: that is a Slicer's slice (phase_4.md §4.6). The Slicer and the sample-pool
// channel are this class under another type name and parameter defaults; a parameter
// table that carries `choke` after the Sampler's own makes a new note fade out the
// voices already sounding, as a slicer's pads and a pool's one-shots expect.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Envelope.h"
#include "engine/dsp/Interpolate.h"
#include "engine/graph/VoicePool.h"
#include "engine/instruments/Instrument.h"
#include "engine/instruments/sampler/ZoneSet.h"

namespace adx::instruments {

struct SamplerVoice {
    const SamplerZone* zone;
    /// Position in the sample's own frames.
    double position;
    /// Ping-pong direction: +1 or -1.
    double direction;
    float velocityGain;
    /// 1 while playing; ramps to 0 once a choking note or a slice end cuts the voice.
    float cut;
    bool released;
    bool tailStarted;
    dsp::Envelope env;
    dsp::EnvelopeShape shape;
};

class SamplerInstrument : public Instrument<SamplerVoice>, public ZoneSet {
public:
    using Instrument::Instrument;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "sampler";
    }

    void reset() noexcept override;

protected:
    void prepareInstrument(const graph::PrepareInfo& info) override;
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;

private:
    /// The zone a note plays, round robin applied; null when none matches.
    [[nodiscard]] const SamplerZone* choose(std::uint8_t key, std::uint8_t velocity) noexcept;

    std::array<std::uint32_t, 256> m_roundRobin{};
    /// The most recent note's voice: the one a choke spares.
    graph::VoiceKey m_latest{};
    const dsp::SincTable* m_sinc{nullptr};
};

} // namespace adx::instruments
