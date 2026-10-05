// FM: six operators, 32 algorithms (phase_4.md §4.8).
//
// An algorithm is a routing matrix, not 32 hard-coded graphs: for each operator, the
// set of operators that modulate it, whether it is a carrier, and where the one
// feedback loop runs (fmAlgorithm()). The 32 are the DX7's. In all of them modulation
// runs from a higher-numbered operator to a lower one, so evaluating operator 6 down to
// operator 1 each sample sees every modulator's current output - except through the
// feedback loop, which reads its source's previous two outputs averaged, as the DX7
// does, and so is stable at any setting.
//
// Each operator is a sine with its own ratio (or fixed frequency), detune, level,
// velocity sensitivity and ADSR. Phase modulation, in turns: a modulator at level 1
// moves its target's phase by kFmModIndex turns (about 12.6 radians, the DX7's range).
// The voice ends when every carrier's envelope has.
#pragma once

#include <array>
#include <cstdint>

#include "engine/dsp/Envelope.h"
#include "engine/instruments/Instrument.h"
#include "engine/instruments/fm/FmParams.h"

namespace adx::instruments {

inline constexpr float kFmModIndex = 2.0F;
/// The feedback loop at 1: half a turn of self-modulation.
inline constexpr float kFmFeedbackIndex = 0.5F;

struct FmAlgorithm {
    /// modulators[op]: bit m set when operator m modulates operator op (0 = op 1).
    std::array<std::uint8_t, kFmOperators> modulators;
    /// Bit op set when operator op is heard.
    std::uint8_t carriers;
    /// The feedback loop: `feedbackFrom`'s output feeds `feedbackTo`'s phase.
    std::uint8_t feedbackFrom;
    std::uint8_t feedbackTo;
};

/// Algorithm `number`, 1..32 (clamped).
[[nodiscard]] const FmAlgorithm& fmAlgorithm(int number) noexcept;

struct FmVoice {
    std::array<float, kFmOperators> phase;
    std::array<float, kFmOperators> output;
    std::array<float, kFmOperators> level;
    std::array<dsp::Envelope, kFmOperators> env;
    std::array<dsp::EnvelopeShape, kFmOperators> shape;
    /// The feedback source's last two outputs.
    float feedback1;
    float feedback2;
    bool released;
};

class FmInstrument final : public Instrument<FmVoice> {
public:
    using Instrument::Instrument;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "fm";
    }

protected:
    void startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                    const graph::VoiceRender& render) noexcept override;
    bool renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const graph::VoiceRender& render) noexcept override;
    [[nodiscard]] float portamentoSeconds(std::span<const float> params) const noexcept override;
};

} // namespace adx::instruments
