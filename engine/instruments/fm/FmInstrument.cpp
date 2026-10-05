#include "engine/instruments/fm/FmInstrument.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "engine/dsp/Math.h"

namespace adx::instruments {
namespace {

using O = FmOpParam;

/// Operator n (1-based, as the DX7 charts number them) as a bit.
constexpr std::uint8_t op(int n) noexcept {
    return static_cast<std::uint8_t>(1U << static_cast<unsigned>(n - 1));
}

/// One algorithm from its modulation edges - "21 65" is operator 2 modulating
/// operator 1 and 6 modulating 5 - its carriers, and its feedback loop.
constexpr FmAlgorithm algorithm(std::string_view edges, std::uint8_t carriers, int feedbackFrom,
                                int feedbackTo) noexcept {
    FmAlgorithm a{};
    for (std::size_t i = 0; i + 1 < edges.size(); i += 3) {
        const int from = edges[i] - '0';
        const int to = edges[i + 1] - '0';
        a.modulators[static_cast<std::size_t>(to - 1)] |= op(from);
    }
    a.carriers = carriers;
    a.feedbackFrom = static_cast<std::uint8_t>(feedbackFrom - 1);
    a.feedbackTo = static_cast<std::uint8_t>(feedbackTo - 1);
    return a;
}

// The DX7's 32. Carriers sit on the bottom row of the charts; feedback is the loop
// drawn on each (a self-loop on one operator, or across two or three).
constexpr std::array<FmAlgorithm, kFmAlgorithms> kAlgorithms{
    algorithm("21 65 54 43", op(1) | op(3), 6, 6),                      // 1
    algorithm("21 65 54 43", op(1) | op(3), 2, 2),                      // 2
    algorithm("32 21 65 54", op(1) | op(4), 6, 6),                      // 3
    algorithm("32 21 65 54", op(1) | op(4), 4, 6),                      // 4
    algorithm("21 43 65", op(1) | op(3) | op(5), 6, 6),                 // 5
    algorithm("21 43 65", op(1) | op(3) | op(5), 5, 6),                 // 6
    algorithm("21 43 53 65", op(1) | op(3), 6, 6),                      // 7
    algorithm("21 43 53 65", op(1) | op(3), 4, 4),                      // 8
    algorithm("21 43 53 65", op(1) | op(3), 2, 2),                      // 9
    algorithm("32 21 54 64", op(1) | op(4), 3, 3),                      // 10
    algorithm("32 21 54 64", op(1) | op(4), 6, 6),                      // 11
    algorithm("21 43 53 63", op(1) | op(3), 2, 2),                      // 12
    algorithm("21 43 53 63", op(1) | op(3), 6, 6),                      // 13
    algorithm("21 43 54 64", op(1) | op(3), 6, 6),                      // 14
    algorithm("21 43 54 64", op(1) | op(3), 2, 2),                      // 15
    algorithm("21 31 43 51 65", op(1), 6, 6),                           // 16
    algorithm("21 31 43 51 65", op(1), 2, 2),                           // 17
    algorithm("21 31 41 54 65", op(1), 3, 3),                           // 18
    algorithm("32 21 64 65", op(1) | op(4) | op(5), 6, 6),              // 19
    algorithm("31 32 54 64", op(1) | op(2) | op(4), 3, 3),              // 20
    algorithm("31 32 64 65", op(1) | op(2) | op(4) | op(5), 3, 3),      // 21
    algorithm("21 63 64 65", op(1) | op(3) | op(4) | op(5), 6, 6),      // 22
    algorithm("32 64 65", op(1) | op(2) | op(4) | op(5), 6, 6),         // 23
    algorithm("63 64 65", op(1) | op(2) | op(3) | op(4) | op(5), 6, 6), // 24
    algorithm("64 65", op(1) | op(2) | op(3) | op(4) | op(5), 6, 6),    // 25
    algorithm("32 54 64", op(1) | op(2) | op(4), 6, 6),                 // 26
    algorithm("32 54 64", op(1) | op(2) | op(4), 3, 3),                 // 27
    algorithm("21 54 43", op(1) | op(3) | op(6), 5, 5),                 // 28
    algorithm("43 65", op(1) | op(2) | op(3) | op(5), 6, 6),            // 29
    algorithm("54 43", op(1) | op(2) | op(3) | op(6), 5, 5),            // 30
    algorithm("65", op(1) | op(2) | op(3) | op(4) | op(5), 6, 6),       // 31
    algorithm("", 0x3F, 6, 6),                                          // 32
};

[[nodiscard]] float opParam(const graph::VoiceRender& render, std::uint32_t o, O field,
                            std::uint32_t frame = 0) noexcept {
    return render.paramAt(fmOpParam(o, field), frame);
}

[[nodiscard]] int algorithmNumber(const graph::VoiceRender& render) noexcept {
    return static_cast<int>(
        std::floor(render.param(static_cast<std::uint32_t>(FmParam::Algorithm)) + 0.5F));
}

} // namespace

const FmAlgorithm& fmAlgorithm(int number) noexcept {
    return kAlgorithms[static_cast<std::size_t>(std::clamp(number, 1, 32) - 1)];
}

float FmInstrument::portamentoSeconds(std::span<const float> params) const noexcept {
    const auto at = static_cast<std::uint32_t>(FmParam::Glide);
    return at < params.size() ? std::max(0.0F, params[at]) : 0.0F;
}

void FmInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                              const graph::VoiceRender& render) noexcept {
    FmVoice& state = stateOf(voice);
    const float velocity = static_cast<float>(voice.velocity) / 127.0F;
    for (std::uint32_t o = 0; o < kFmOperators; ++o) {
        state.phase[o] = 0.0F;
        state.output[o] = 0.0F;
        const float sensitivity = std::clamp(opParam(render, o, O::Velocity), 0.0F, 1.0F);
        state.level[o] = std::clamp(opParam(render, o, O::Level), 0.0F, 1.0F) *
                         (1.0F - sensitivity + (sensitivity * velocity));
        dsp::EnvelopeShape& shape = state.shape[o];
        shape.attackSeconds = opParam(render, o, O::Attack);
        shape.decaySeconds = opParam(render, o, O::Decay);
        shape.sustain = opParam(render, o, O::Sustain);
        shape.releaseSeconds = opParam(render, o, O::Release);
        state.env[o].reset();
        state.env[o].trigger(shape, sampleRate());
    }
    state.feedback1 = 0.0F;
    state.feedback2 = 0.0F;
    state.released = false;
    voice.level = 0.0F;
}

// NOLINTNEXTLINE(readability-function-size) - one voice: six operators through a matrix.
bool FmInstrument::renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                               const graph::VoiceRender& render) noexcept {
    FmVoice& state = stateOf(voice);
    if (voice.phase == graph::VoicePhase::Released && !state.released) {
        state.released = true;
        for (std::uint32_t o = 0; o < kFmOperators; ++o) {
            state.env[o].release(state.shape[o], sampleRate());
        }
    }
    const FmAlgorithm& algo = fmAlgorithm(algorithmNumber(render));
    const float inverseRate = 1.0F / static_cast<float>(sampleRate());
    const float feedback =
        std::clamp(render.param(static_cast<std::uint32_t>(FmParam::Feedback)), 0.0F, 1.0F) *
        kFmFeedbackIndex;
    int carrierCount = 0;
    for (std::uint32_t o = 0; o < kFmOperators; ++o) {
        carrierCount += static_cast<int>((algo.carriers >> o) & 1U);
    }
    // Carriers add like partly correlated signals: between sqrt(n) and n.
    const float norm = 1.0F / std::sqrt(static_cast<float>(std::max(carrierCount, 1)));

    // Per operator, constant over the call: ratio and detune, or a fixed frequency.
    std::array<float, kFmOperators> ratio{};
    std::array<float, kFmOperators> fixedHz{};
    for (std::uint32_t o = 0; o < kFmOperators; ++o) {
        const float detune = dsp::centsToRatio(opParam(render, o, O::Detune));
        if (opParam(render, o, O::Fixed) >= 0.5F) {
            fixedHz[o] = opParam(render, o, O::Freq) * detune;
        } else {
            ratio[o] = opParam(render, o, O::Ratio) * detune;
        }
    }
    const auto levelIndex = static_cast<std::uint32_t>(FmParam::Level);
    const bool levelAutomated =
        levelIndex < render.automation.size() && render.automation[levelIndex] != nullptr;
    const float level = dsp::dbToGainF(render.param(levelIndex));
    const auto pitch = static_cast<float>(voice.pitch);

    bool alive = true;
    float carrierEnv = 0.0F;
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        const float cents = i < render.pitchCents.size() ? render.pitchCents[i] : 0.0F;
        const float noteHz = dsp::midiToHz(pitch + (cents * 0.01F));
        float out = 0.0F;
        bool sounding = false;
        carrierEnv = 0.0F;
        // Operator 6 first: every modulator is evaluated before what it modulates.
        for (std::uint32_t k = kFmOperators; k-- > 0;) {
            float modulation = 0.0F;
            const std::uint8_t mods = algo.modulators[k];
            for (std::uint32_t m = 0; m < kFmOperators; ++m) {
                if (((mods >> m) & 1U) != 0) {
                    modulation += state.output[m];
                }
            }
            modulation *= kFmModIndex;
            if (k == algo.feedbackTo) {
                modulation += feedback * 0.5F * (state.feedback1 + state.feedback2);
            }
            const float env = state.env[k].next(state.shape[k]);
            const float hz = fixedHz[k] > 0.0F ? fixedHz[k] : noteHz * ratio[k];
            float phase = state.phase[k] + modulation;
            phase -= std::floor(phase);
            state.output[k] = dsp::sinTurnsF(phase) * env * state.level[k];
            state.phase[k] += hz * inverseRate;
            state.phase[k] -= std::floor(state.phase[k]);
            if (((algo.carriers >> k) & 1U) != 0) {
                out += state.output[k];
                carrierEnv = std::max(carrierEnv, env);
                sounding = sounding || !state.env[k].finished();
            }
        }
        state.feedback2 = state.feedback1;
        state.feedback1 = state.output[algo.feedbackFrom];

        const float gain =
            norm * (levelAutomated ? dsp::dbToGainF(render.paramAt(levelIndex, i)) : level);
        left[i] = out * gain;
        right[i] = out * gain;
        if (!sounding) {
            alive = false;
        }
    }
    voice.level = carrierEnv;
    return alive;
}

} // namespace adx::instruments
