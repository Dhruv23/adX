// The `physical` instrument (phase_12.md §4.4, closes G4): exciter -> resonator -> body.
//
//   pluck  extended Karplus-Strong. A noise burst, low-passed by brightness and combed by
//          the pick position, circulates in a delay line through a loss filter and an
//          optional dispersion allpass. The loop is tuned exactly: the loss filter's and
//          the dispersion's phase delay at the fundamental are subtracted from the
//          period, and a first-order allpass is solved for what is left, so a note is in
//          tune to well under 3 cents from A0 to C8 (physical_pluck_pitch). The loop gain
//          makes the fundamental fall 60 dB in string.decay at C4 (physical_pluck_decay).
//   bow    a waveguide string split at the bow, driven through a friction table (Smith's
//          bowed string, after STK's Bowed): pressure sets the table's slope, speed the
//          bow velocity, the envelope its attack and release.
//   modal  up to eight two-pole resonators tuned to a shipped mode table (ModeTables.h),
//          struck by a half-sine mallet whose contact time hardness sets; upper modes die
//          faster by decayTilt.
//   tube   a bore with a jet (flute) or a reed (clarinet; saxophone as a longer bore),
//          after STK's Flute and Clarinet; breath.pressure through the envelope drives it.
//
// The body is three fixed resonances per type (guitar, violin, cello, pan skirt), mixed
// in by body.mix. The plan's commuted body IR would make a node load a file; a few
// resonances cost nothing per voice and need none (phase_12.md §11).
//
// Every voice owns two delay lines in one buffer the node allocates in prepare.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Envelope.h"
#include "engine/dsp/Noise.h"
#include "engine/instruments/Instrument.h"
#include "engine/instruments/physical/ModeTables.h"
#include "engine/instruments/physical/PhysicalParams.h"
#include "engine/rt/OwnedArray.h"

namespace adx::instruments {

/// A first-order allpass's phase delay at `omega` (radians per sample), samples.
[[nodiscard]] double allpassDelay(double eta, double omega) noexcept;
/// The coefficient whose phase delay at `omega` is `delay` (0.3..1.7 samples).
[[nodiscard]] double allpassFor(double delay, double omega) noexcept;
/// The one-pole low-pass y = (1-a) x + a y1's phase delay at `omega`.
[[nodiscard]] double onePoleDelay(double a, double omega) noexcept;
/// ... and its magnitude there.
[[nodiscard]] double onePoleGain(double a, double omega) noexcept;

struct PhysicalVoice {
    PhysicalModel model;
    std::uint32_t write;
    /// The note's frequency the lines are tuned for, Hz.
    float tunedHz;

    // --- pluck ------------------------------------------------------------------------
    std::uint32_t integerDelay;
    float eta;
    float fracX1;
    float fracY1;
    float lossA;
    float lossState;
    float loopGain;
    float dispersion;
    std::array<float, 2> dispX1;
    std::array<float, 2> dispY1;
    std::uint32_t exciteLeft;
    std::uint32_t pickDelay;
    float exciteLp;
    float exciteCoefficient;
    float thunk;

    // --- bow and tube -------------------------------------------------------------------
    float lengthA;
    float lengthB;
    float filterState;
    float dcX1;
    float dcY1;

    // --- modal --------------------------------------------------------------------------
    std::uint32_t modeCount;
    std::array<float, kMaxModes> modeB1;
    std::array<float, kMaxModes> modeB2;
    std::array<float, kMaxModes> modeGain;
    std::array<float, kMaxModes> modeY1;
    std::array<float, kMaxModes> modeY2;
    std::array<float, kMaxModes> modeRatio;
    std::uint32_t strikeLeft;
    std::uint32_t strikeLength;
    float decayScale;

    // --- body ---------------------------------------------------------------------------
    std::array<float, 3> bodyB1;
    std::array<float, 3> bodyB2;
    std::array<float, 3> bodyGain;
    std::array<float, 3> bodyY1;
    std::array<float, 3> bodyY2;
    std::uint8_t body;

    dsp::WhiteNoise noise;
    dsp::Envelope env;
    dsp::EnvelopeShape shape;
    ModVoice mod;
    float velocity;
    std::uint32_t quietFrames;
    bool released;
};

class PhysicalInstrument final : public Instrument<PhysicalVoice> {
public:
    using Instrument::Instrument;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "physical";
    }

protected:
    void prepareInstrument(const graph::PrepareInfo& info) override;
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;
    [[nodiscard]] float portamentoSeconds(std::span<const float> params) const noexcept override;
    [[nodiscard]] std::uint8_t voiceMode(std::span<const float> params) const noexcept override;
    [[nodiscard]] bool glideAlways(std::span<const float> params) const noexcept override;
    void retriggerVoice(graph::Voice& voice, const graph::BlockEvent& event,
                        const graph::VoiceRender& render) noexcept override;

private:
    [[nodiscard]] std::span<float> lineA(const graph::Voice& voice) noexcept;
    [[nodiscard]] std::span<float> lineB(const graph::Voice& voice) noexcept;
    /// Tunes the model's lines (or modes) for `hz`, reading its knobs at `frame`.
    void tune(PhysicalVoice& state, const graph::VoiceRender& render, std::uint32_t frame, float hz,
              const dsp::ModFrame& mod) noexcept;
    void setBody(PhysicalVoice& state, std::uint8_t type) const noexcept;
    [[nodiscard]] float pluck(PhysicalVoice& state, std::span<float> line,
                              std::span<float> pick) noexcept;
    [[nodiscard]] float bow(PhysicalVoice& state, std::span<float> neck, std::span<float> bridge,
                            float velocity, float pressure) noexcept;
    [[nodiscard]] float modal(PhysicalVoice& state, float velocity) noexcept;
    [[nodiscard]] float tube(PhysicalVoice& state, std::span<float> bore, std::span<float> jet,
                             float breath, float noise, float stiffness, int kind) noexcept;
    [[nodiscard]] float bodyOf(PhysicalVoice& state, float x, float mix) noexcept;

    rt::OwnedArray<float> m_lines;
    std::size_t m_lineSize{4096};
    std::size_t m_lineMask{4095};
};

} // namespace adx::instruments
