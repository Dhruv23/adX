// The Sampler: zones, loop modes, velocity layers and round robin over pool samples
// (phase_4.md §4.5).
//
// Zones are baked into the node when it is built, already resolved to sample handles
// (SamplerSetup.cpp, main thread), and never change after prepare: a zone edit makes a
// new node, the way a Limiter's lookahead does (instruments::configMatches). So the
// audio thread reads an immutable table and a handle per zone, and the buffers behind
// the handles are pinned for the node's lifetime by an object it cannot see the type
// of - a node is destroyed by the Reaper, on the main thread, which is where the pins
// are released.
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
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Envelope.h"
#include "engine/dsp/Interpolate.h"
#include "engine/format/audio/SampleView.h"
#include "engine/instruments/Instrument.h"
#include "engine/project/SampleZone.h"
#include "engine/rt/OwnedArray.h"

namespace adx::instruments {

/// One zone as the audio thread reads it: the project's zone, plus its sample.
struct SamplerZone {
    const format::SampleHandle* sample;
    project::SampleZone zone;
    /// dB to linear, and the pan split into left and right gains, precomputed.
    float gainLeft;
    float gainRight;
};

struct SamplerVoice {
    const SamplerZone* zone;
    /// Position in the sample's own frames.
    double position;
    /// Ping-pong direction: +1 or -1.
    double direction;
    float velocityGain;
    bool released;
    bool tailStarted;
    dsp::Envelope env;
    dsp::EnvelopeShape shape;
};

/// The pins: whatever keeps the zones' samples alive. Defined, created and destroyed
/// in SamplerSetup.cpp, on the main thread.
struct SamplerPins;
void destroySamplerPins(SamplerPins* pins) noexcept;

class SamplerInstrument final : public Instrument<SamplerVoice> {
public:
    using Instrument::Instrument;
    ~SamplerInstrument() override;
    SamplerInstrument(const SamplerInstrument&) = delete;
    SamplerInstrument& operator=(const SamplerInstrument&) = delete;
    SamplerInstrument(SamplerInstrument&&) = delete;
    SamplerInstrument& operator=(SamplerInstrument&&) = delete;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "sampler";
    }

    /// Main thread, before prepare(). Takes ownership of `pins`.
    void setZones(rt::OwnedArray<SamplerZone> zones, SamplerPins* pins) noexcept;
    [[nodiscard]] std::span<const SamplerZone> zones() const noexcept {
        return m_zones.view();
    }
    [[nodiscard]] const SamplerPins* pins() const noexcept {
        return m_pins;
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

    rt::OwnedArray<SamplerZone> m_zones;
    SamplerPins* m_pins{nullptr};
    std::array<std::uint32_t, 256> m_roundRobin{};
    const dsp::SincTable* m_sinc{nullptr};
};

} // namespace adx::instruments
