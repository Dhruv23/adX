// Vocoder and FormantFilter (phase_4.md §4.9, Tranche C).
//
// Vocoder: a channel vocoder. The input is the modulator: a bank of 16-32 band-passes,
// log-spaced from 100 Hz to 8 kHz, each followed by an envelope follower (`attack`,
// `release`). The carrier runs through a matching bank whose bands, scaled by
// `formantShift`, are gained by those envelopes and summed. The carrier is the
// sidechain - Phase 3's explicit sidechain edge, the same mechanism as the
// Compressor's - or built in: a band-limited saw at `carrierPitch`, noise, or both, so
// it works on one track. (§4.9 sketches the saw "tracking MIDI via the host channel";
// an effect sees no notes, so the pitch is a parameter, and automatable.) What matters
// more than band count for intelligibility: `bandwidth`, and the sibilance path - the
// modulator above 5 kHz, which no carrier reproduces, mixed straight to the output by
// `sibilance`. `freeze` holds the envelopes. The filter bank is IIR, so its delay
// varies with frequency and is under 2 ms above 100 Hz; it declares no latency.
//
// FormantFilter: vowel morphing as an insert - dsp::FormantBank between two vowels,
// the morph automatable and swept by an optional LFO, the formants shiftable.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/FormantBank.h"
#include "engine/dsp/Lfo.h"
#include "engine/dsp/Noise.h"
#include "engine/dsp/Oscillator.h"
#include "engine/dsp/SvFilter.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Params.h"

namespace adx::effects {

inline constexpr std::size_t kVocoderMaxBands = 32;

enum class VocoderCarrier : std::uint8_t { Sidechain, Saw, Noise, SawNoise };

enum class VocoderParam : std::uint32_t {
    Bands,
    Carrier,
    CarrierPitch,
    Attack,
    Release,
    Bandwidth,
    FormantShift,
    Sibilance,
    Freeze,
    Gain,
    Count,
};
inline constexpr auto kVocoderParams = std::to_array<project::ParamDescriptor>({
    param("bands", 16.0F, 32.0F, 24.0F, project::Unit::Count, project::ScaleKind::Stepped),
    choice("carrier", 3.0F, 1.0F),
    param("carrierPitch", 30.0F, 1000.0F, 110.0F, project::Unit::Hertz,
          project::ScaleKind::Logarithmic),
    param("attack", 0.5F, 100.0F, 5.0F, project::Unit::Milliseconds,
          project::ScaleKind::Logarithmic),
    param("release", 5.0F, 500.0F, 50.0F, project::Unit::Milliseconds,
          project::ScaleKind::Logarithmic),
    // Each band's width relative to its spacing: 1 is the bands just touching.
    param("bandwidth", 0.25F, 4.0F, 1.0F, project::Unit::Ratio, project::ScaleKind::Logarithmic),
    param("formantShift", -12.0F, 12.0F, 0.0F, project::Unit::Semitones),
    perFrame("sibilance", 0.0F, 1.0F, 0.3F, project::Unit::Normalized),
    param("freeze", 0.0F, 1.0F, 0.0F, project::Unit::Boolean, project::ScaleKind::Stepped),
    perFrame("gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
});

enum class FormantFilterParam : std::uint32_t {
    VowelA,
    VowelB,
    Morph,
    LfoRate,
    LfoDepth,
    Shift,
    Set,
    Gain,
    Count,
};
inline constexpr auto kFormantFilterParams = std::to_array<project::ParamDescriptor>({
    // a e i o u.
    choice("vowelA", 4.0F, 0.0F),
    choice("vowelB", 4.0F, 3.0F),
    perFrame("morph", 0.0F, 1.0F, 0.0F, project::Unit::Normalized),
    param("lfoRate", 0.01F, 20.0F, 0.5F, project::Unit::Hertz, project::ScaleKind::Logarithmic),
    param("lfoDepth", 0.0F, 1.0F, 0.0F, project::Unit::Normalized),
    param("shift", -12.0F, 12.0F, 0.0F, project::Unit::Semitones),
    // 0 iteration one's three formants; 1 the five-formant tenor table.
    choice("set", 1.0F, 1.0F),
    perFrame("gain", -24.0F, 24.0F, 0.0F, project::Unit::Decibels),
});

class Vocoder final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "Vocoder";
    }
    [[nodiscard]] bool runsWhileBypassed() const noexcept override {
        return true;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& info) override;
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;
    [[nodiscard]] bool usesSidechain() const noexcept override {
        return true;
    }

private:
    void refresh(const EffectContext& context, std::uint32_t frame) noexcept;

    std::array<dsp::SvFilter, kVocoderMaxBands> m_analysis{};
    std::array<std::array<dsp::SvFilter, kVocoderMaxBands>, 2> m_synthesis{};
    std::array<dsp::SvfCoefficients, kVocoderMaxBands> m_analysisCoefficients{};
    std::array<dsp::SvfCoefficients, kVocoderMaxBands> m_synthesisCoefficients{};
    std::array<float, kVocoderMaxBands> m_envelope{};
    dsp::SvFilter m_sibilanceFilter;
    dsp::SvfCoefficients m_sibilanceCoefficients{};
    dsp::Oscillator m_saw;
    dsp::WhiteNoise m_noise{0x70C0DEU};
    std::size_t m_bands{24};
    VocoderCarrier m_carrier{VocoderCarrier::Saw};
    float m_increment{0.0F};
    float m_attack{0.0F};
    float m_release{0.0F};
    bool m_freeze{false};
};

class FormantFilter final : public Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "FormantFilter";
    }
    [[nodiscard]] MixLaw mixLaw() const noexcept override {
        return MixLaw::Linear;
    }

protected:
    void prepareEffect(const graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override;
    void processWet(std::span<const float> inLeft, std::span<const float> inRight,
                    std::span<float> outLeft, std::span<float> outRight,
                    const EffectContext& context) noexcept override;

private:
    std::array<dsp::FormantBank, 2> m_bank{};
    dsp::Lfo m_lfo{0xF0E3A7U};
    float m_increment{0.0F};
    float m_depth{0.0F};
};

} // namespace adx::effects
