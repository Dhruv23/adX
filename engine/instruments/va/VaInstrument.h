// VA: the subtractive synth (phase_4.md §4.4).
//
// Two oscillators and a sub, each band-limited (dsp/WaveTable.h's mipmaps: v1's
// polyBLEP measures -21 dBc of aliasing at 10 kHz, the tables meet -60), stacked up to
// eight unison copies detuned symmetrically and spread across the stereo field; noise;
// then a filter that is a 4-pole ZDF ladder or a state-variable filter in one of four
// modes - "because a ladder is what a subtractive synth is for" - driven by its own
// envelope, the key and the velocity; an amplitude envelope; and two LFOs, each routed
// to one of pitch, cutoff, amplitude, pan or pulse width.
//
// Filter coefficients follow the voice's 32-frame control grid (Instrument.h), as the
// Additive's do. Pitch - glides, portamento, an LFO on pitch - is per frame.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Envelope.h"
#include "engine/dsp/LadderFilter.h"
#include "engine/dsp/Lfo.h"
#include "engine/dsp/Noise.h"
#include "engine/dsp/Oscillator.h"
#include "engine/dsp/SvFilter.h"
#include "engine/instruments/Instrument.h"
#include "engine/instruments/va/VaParams.h"

namespace adx::instruments {

struct VaVoice {
    std::array<dsp::Oscillator, kVaMaxUnison> osc1;
    std::array<dsp::Oscillator, kVaMaxUnison> osc2;
    dsp::Oscillator sub;
    dsp::WhiteNoise noise;
    std::uint32_t unison;
    /// Each copy's detune as a frequency ratio, and its place in the stereo field.
    std::array<float, kVaMaxUnison> detune;
    std::array<float, kVaMaxUnison> panLeft;
    std::array<float, kVaMaxUnison> panRight;
    float unisonNorm;

    std::array<dsp::LadderFilter, 2> ladder;
    std::array<dsp::SvFilter, 2> svf;
    dsp::LadderCoefficients ladderCoefficients;
    dsp::SvfCoefficients svfCoefficients;
    float filterDrive;
    VaFilterType filterType;

    dsp::Envelope ampEnv;
    dsp::Envelope filterEnv;
    dsp::EnvelopeShape ampShape;
    dsp::EnvelopeShape filterShape;
    std::array<dsp::Lfo, 2> lfo;
    float velocity;
    bool released;
};

class VaInstrument final : public Instrument<VaVoice> {
public:
    using Instrument::Instrument;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "va";
    }

protected:
    void prepareInstrument(const graph::PrepareInfo& info) override;
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;
    [[nodiscard]] float portamentoSeconds(std::span<const float> params) const noexcept override;

private:
    /// The block-rate parameters at `frame`: unison layout, envelopes, filter.
    void refresh(graph::Voice& voice, VaVoice& state, const graph::VoiceRender& render,
                 std::uint32_t frame, float lfoCutoffOctaves) noexcept;
};

} // namespace adx::instruments
