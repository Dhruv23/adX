// DrumSynth: five synthesis models rather than one parametric compromise
// (phase_4.md §4.7).
//
//   kick   a sine swept down from `pitchDrop` semitones above its resting pitch, a
//          click (a 2 ms burst of high-passed noise), an optional sine sub, then drive
//   snare  a 180/330 Hz tone pair with a short pitch dip, plus band-passed noise
//   hat    six band-limited squares at the TR-808's metallic ratios, band-passed and
//          high-passed, through a fast VCA
//   clap   three noise bursts 11 ms apart and a longer "reverb" tail, band-passed
//   tom    the kick's sweep, shallower and higher, with a little noise on the skin
//
// and the 808's percussion (phase_12.md §4.5):
//
//   cowbell  oscillators 5 and 6 of the hat's bank (540, 800 Hz), band-passed at 2.6 kHz,
//            a 15 ms first stage then `decay`
//   cymbal   all six, split: a short 7 kHz high band and a long 3.4 kHz body
//   rim      a ping at 1.7 kHz (a damped sine, the bridged-T's ring) and a 1 ms click
//   clave    the same ping at 2.5 kHz, 20-60 ms
//   shaker   high-passed noise, a 5 ms rise then `decay`; `tone` is the cutoff
//   conga    the tom with a quick pitch blip and a narrower skin; in a kit, three tunings
//
// A hit is one-shot: note-off does nothing, and the voice ends when its level has
// fallen 80 dB. Parameters latch at the hit (DrumSynthParams.h). Every sound is a
// function of the voice's identity and its parameters - noise is seeded from the
// voice key - so offline and realtime renders are the same.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Noise.h"
#include "engine/dsp/Oscillator.h"
#include "engine/dsp/SvFilter.h"
#include "engine/instruments/Instrument.h"
#include "engine/instruments/drumsynth/DrumSynthParams.h"

namespace adx::instruments {

struct DrumVoice {
    DrumModel model;
    /// Resting frequency of the body, Hz.
    float frequency;
    /// Per-sample decay multipliers and current levels.
    float amp;
    float ampDecay;
    float toneAmp;
    float toneDecay;
    float sweep;
    float sweepDecay;
    float click;
    float clickDecay;
    float pitchDrop;
    /// Phases, in turns: body, second tone, sub.
    float phase;
    float phase2;
    float subPhase;
    std::array<dsp::Oscillator, 6> metal;
    dsp::WhiteNoise noise;
    dsp::SvFilter filter;
    dsp::SvFilter clickFilter;
    dsp::SvFilter highPass;
    dsp::SvfCoefficients filterCoefficients;
    dsp::SvfCoefficients clickCoefficients;
    dsp::SvfCoefficients highPassCoefficients;
    float noiseLevel;
    float clickLevel;
    float subLevel;
    float drive;
    float driveNorm;
    float velocity;
    /// A second, faster envelope stage (the cowbell's first 15 ms, the cymbal's high
    /// band, the shaker's rise).
    float fast;
    float fastDecay;
    int character;
    std::uint32_t age;
};

class DrumSynth final : public Instrument<DrumVoice> {
public:
    using Instrument::Instrument;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "drumsynth";
    }

protected:
    void prepareInstrument(const graph::PrepareInfo& info) override;
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;
};

/// The model a key plays in Kit mode: General MIDI's drum map, folded.
[[nodiscard]] DrumModel kitModel(std::uint8_t key) noexcept;

} // namespace adx::instruments
