// Granular: a grain scheduler over a sample or a built-in tone (phase_4.md §4.8).
//
// Each voice runs its own scheduler: every 1/density seconds it starts a grain of
// `size` ms at the scan position plus a random spray, at the note's pitch plus a
// random jitter, panned at random within `spread`, shaped by the chosen window. Grains
// live in a fixed per-voice array (kGrainsPerVoice), preallocated in prepare() like
// the archive's particle pool, whose no-per-frame-allocation discipline FINAL_PLAN
// §3.1 credits. Randomness is xorshift seeded from the voice's identity, and the
// scheduler counts the voice's own frames, so a voice is the same offline and
// realtime and at every block size.
//
// The sample comes from the channel's zones (ZoneSet.h), read with linear
// interpolation - a grain is short and windowed, and at 32 grains a voice the 8-point
// sinc would cost more than the voice. The zone's root key sets the pitch at which a
// grain plays the sample unshifted.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Envelope.h"
#include "engine/dsp/Noise.h"
#include "engine/instruments/Instrument.h"
#include "engine/instruments/granular/GranularParams.h"
#include "engine/instruments/sampler/ZoneSet.h"

namespace adx::instruments {

struct Grain {
    /// Read position: sample frames for a sample source, turns for a tone.
    double position;
    double increment;
    std::uint32_t age;
    std::uint32_t length;
    float gainLeft;
    float gainRight;
    bool active;
};

struct GranularVoice {
    std::array<Grain, kGrainsPerVoice> grains;
    const SamplerZone* zone;
    dsp::WhiteNoise random;
    dsp::Envelope env;
    dsp::EnvelopeShape shape;
    /// Frames until the next grain starts.
    double untilNext;
    /// Frames since the note started, for the scan.
    std::uint64_t frames;
    float velocity;
    bool released;
};

class GranularInstrument final : public Instrument<GranularVoice>, public ZoneSet {
public:
    using Instrument::Instrument;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "granular";
    }

protected:
    void prepareInstrument(const graph::PrepareInfo& info) override;
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;

private:
    void spawn(graph::Voice& voice, GranularVoice& state, const graph::VoiceRender& render,
               std::uint32_t frame) noexcept;
};

/// The window's value at `t` in [0, 1].
[[nodiscard]] float grainWindow(GrainWindow window, float t) noexcept;

} // namespace adx::instruments
