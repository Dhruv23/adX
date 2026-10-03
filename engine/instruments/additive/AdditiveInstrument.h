// The Additive instrument: iteration one's voice, ported (phase_4.md §4.3).
//
// _archive/src-cpp/src/AudioEngine.cpp's per-voice synthesis, extracted from the
// scheduler it was tangled with and kept in its signal order, because that order is
// what suffocation.adx was written against:
//
//   fundamental <- pitch + glide + pitch drop + vibrato           (cents, one exp2)
//   harmonic stack (up to 64 now, each with detune and phase)
//   + sub oscillator + noise + polyBLEP unison saw/square/tri
//   -> formant bank (Classic3, crossfaded by formant.amount)
//   -> amplitude envelope x velocity
//   -> one-pole lowpass with LFO -> resonant SVF with its own envelope
//   -> tanh drive -> centre, plus the unison's stereo width
//
// What changed, and why: envelopes release from where they are rather than from the
// sustain level (v1 clicked); harmonics above Nyquist are skipped rather than aliased;
// filter coefficients are computed on a 32-frame control grid instead of every sample
// (sub-millisecond, and the reason a 16-voice pad fits in a callback); glide is the
// voice base's legato portamento (phase_4.md §4.2); and every transcendental is
// dsp/Math.h's, so the golden hashes hold in every build.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Envelope.h"
#include "engine/dsp/FormantBank.h"
#include "engine/dsp/Noise.h"
#include "engine/dsp/Oscillator.h"
#include "engine/dsp/SvFilter.h"
#include "engine/instruments/Instrument.h"
#include "engine/instruments/additive/AdditiveParams.h"

namespace adx::instruments {

/// v1's limits, kept: seven unison copies, a 5-octave filter envelope sweep, cutoffs
/// at or above 19 kHz meaning "no filter".
inline constexpr std::uint32_t kAdditiveMaxUnison = 7;
inline constexpr float kFilterEnvOctaves = 5.0F;
inline constexpr float kFilterBypassHz = 19000.0F;
/// How much of the unison's L/R spread reaches the output (v1's kUnisonWidthAmount).
inline constexpr float kUnisonWidth = 0.6F;

struct AdditiveVoice {
    std::array<float, kAdditiveHarmonics> phase;
    std::array<float, kAdditiveHarmonics> amplitude;
    /// Harmonic k's frequency multiple, k times its detune ratio.
    std::array<float, kAdditiveHarmonics> ratio;
    /// Harmonics with a non-negligible amplitude, so the per-sample loop skips the rest.
    std::uint32_t topHarmonic;

    std::array<dsp::Oscillator, kAdditiveMaxUnison> unison;
    std::uint32_t unisonCount;
    float unisonDetune;
    /// Each unison copy's frequency ratio and pan, from the detune spread.
    std::array<float, kAdditiveMaxUnison> unisonRatio;
    std::array<float, kAdditiveMaxUnison> unisonPan;
    float unisonPulseWidth;
    std::uint8_t unisonWave;

    float subPhase;
    dsp::WhiteNoise noise;
    /// Paul Kellett's 3-pole pink filter, v1's NOISE type 1.
    std::array<float, 3> pink;

    dsp::Envelope ampEnv;
    dsp::Envelope filterEnv;
    dsp::EnvelopeShape ampShape;
    dsp::EnvelopeShape filterShape;

    dsp::FormantBank formant;
    float formantAmount;

    /// The one-pole lowpass: state, coefficient, LFO phase.
    float lpState;
    float lpCoefficient;
    float lfoPhase;
    bool lpActive;

    dsp::SvFilter svf;
    dsp::SvfCoefficients svfCoefficients;
    std::uint8_t svfType;
    bool svfActive;

    float vibratoPhase;
    float driveGain;
    float driveNorm;
};

class AdditiveInstrument final : public Instrument<AdditiveVoice> {
public:
    using Instrument::Instrument;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "additive";
    }

protected:
    void prepareInstrument(const graph::PrepareInfo& info) override;
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;
    [[nodiscard]] float portamentoSeconds(std::span<const float> params) const noexcept override;

private:
    /// Reads the block-rate parameters at frame `frame` and recomputes everything that
    /// derives from them: harmonic amplitudes, envelope shapes, filter coefficients.
    void refresh(graph::Voice& voice, AdditiveVoice& state, const graph::VoiceRender& render,
                 std::uint32_t frame) noexcept;
};

} // namespace adx::instruments
