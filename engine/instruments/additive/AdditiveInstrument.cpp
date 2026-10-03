#include "engine/instruments/additive/AdditiveInstrument.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/dsp/Math.h"
#include "engine/dsp/Saturate.h"

namespace adx::instruments {
namespace {

using P = AdditiveParam;

[[nodiscard]] constexpr std::uint32_t idx(P param) noexcept {
    return static_cast<std::uint32_t>(param);
}

/// Harmonic `k`'s level in a built-in series: 1 saw, 2 square, 3 organ, 4 bell
/// (phase_4.md §4.3's harmonic-series presets). 0 is "use the harmonic.N levels".
[[nodiscard]] float seriesLevel(int series, std::uint32_t k) noexcept {
    const auto kk = static_cast<float>(k);
    switch (series) {
    case 1:
        return 1.0F / kk;
    case 2:
        return k % 2 == 1 ? 1.0F / kk : 0.0F;
    case 3: {
        // Drawbar-ish: 16', 8', 5 1/3', 4', 2 2/3', 2'.
        switch (k) {
        case 1:
            return 1.0F;
        case 2:
            return 0.8F;
        case 3:
            return 0.6F;
        case 4:
            return 0.5F;
        case 6:
            return 0.35F;
        case 8:
            return 0.25F;
        default:
            return 0.0F;
        }
    }
    case 4:
        return k <= 24 ? dsp::exp2F(-0.5F * (kk - 1.0F)) : 0.0F;
    default:
        return 0.0F;
    }
}

[[nodiscard]] core::Curve curveFrom(const graph::VoiceRender& render, P kind,
                                    std::uint32_t frame) noexcept {
    const std::uint32_t base = idx(kind);
    core::Curve curve;
    const auto k = static_cast<int>(std::lround(render.paramAt(base, frame)));
    curve.kind =
        static_cast<core::CurveKind>(std::clamp(k, 0, static_cast<int>(core::kCurveKindCount) - 1));
    curve.tension = render.paramAt(base + 1, frame);
    curve.c1x = render.paramAt(base + 2, frame);
    curve.c1y = render.paramAt(base + 3, frame);
    curve.c2x = render.paramAt(base + 4, frame);
    curve.c2y = render.paramAt(base + 5, frame);
    return curve;
}

[[nodiscard]] int stepped(const graph::VoiceRender& render, P param, std::uint32_t frame) noexcept {
    return static_cast<int>(std::floor(render.paramAt(idx(param), frame) + 0.5F));
}

[[nodiscard]] std::uint32_t voiceSeed(const graph::Voice& voice) noexcept {
    // From the voice's identity, never from a clock: the same note in the same
    // project makes the same noise offline and realtime (phase_3.md §4.10).
    std::uint32_t h = 2166136261U;
    for (const std::uint32_t part : {voice.key.channelId, voice.key.noteId, voice.key.instance}) {
        h = (h ^ part) * 16777619U;
    }
    return h == 0 ? 1U : h;
}

} // namespace

void AdditiveInstrument::prepareInstrument(const graph::PrepareInfo& /*info*/) {
    dsp::Oscillator::prepareTables();
}

float AdditiveInstrument::portamentoSeconds(std::span<const float> params) const noexcept {
    const std::uint32_t at = idx(P::GlideMs);
    return at < params.size() ? std::max(0.0F, params[at]) * 0.001F : 0.0F;
}

void AdditiveInstrument::refresh(graph::Voice& voice, AdditiveVoice& state,
                                 const graph::VoiceRender& render, std::uint32_t frame) noexcept {
    const auto rate = static_cast<double>(sampleRate());

    // --- the harmonic stack -------------------------------------------------
    const int series = stepped(render, P::HarmonicsSeries, frame);
    const int target = stepped(render, P::MorphTarget, frame);
    const float morph = std::clamp(render.paramAt(idx(P::MorphAmount), frame), 0.0F, 1.0F);
    std::uint32_t top = 0;
    for (std::uint32_t k = 1; k <= kAdditiveHarmonics; ++k) {
        float level =
            series > 0 ? seriesLevel(series, k) : render.paramAt(idx(P::Harmonic1) + k - 1, frame);
        if (target > 0 && morph > 0.0F) {
            level += (seriesLevel(target, k) - level) * morph;
        }
        state.amplitude[k - 1] = level;
        const float detune = render.paramAt(idx(P::HarmonicDetune1) + k - 1, frame);
        state.ratio[k - 1] =
            static_cast<float>(k) * (detune == 0.0F ? 1.0F : dsp::centsToRatio(detune));
        if (std::abs(level) > 0.0001F) {
            top = k;
        }
    }
    state.topHarmonic = top;

    // --- envelopes ----------------------------------------------------------
    state.ampShape.attackSeconds = render.paramAt(idx(P::EnvAttack), frame);
    state.ampShape.decaySeconds = render.paramAt(idx(P::EnvDecay), frame);
    state.ampShape.sustain = std::clamp(render.paramAt(idx(P::EnvSustain), frame), 0.0F, 1.0F);
    state.ampShape.releaseSeconds = render.paramAt(idx(P::EnvRelease), frame);
    state.ampShape.attackCurve = curveFrom(render, P::EnvAttackCurveKind, frame);
    state.ampShape.decayCurve = curveFrom(render, P::EnvDecayCurveKind, frame);
    state.ampShape.releaseCurve = curveFrom(render, P::EnvReleaseCurveKind, frame);
    state.filterShape.attackSeconds = render.paramAt(idx(P::FilterEnvAttack), frame);
    state.filterShape.decaySeconds = render.paramAt(idx(P::FilterEnvDecay), frame);
    state.filterShape.sustain =
        std::clamp(render.paramAt(idx(P::FilterEnvSustain), frame), 0.0F, 1.0F);
    state.filterShape.releaseSeconds = render.paramAt(idx(P::FilterEnvRelease), frame);

    // --- unison -------------------------------------------------------------
    state.unisonWave =
        static_cast<std::uint8_t>(std::clamp(stepped(render, P::OscWave, frame), 0, 3));
    state.unisonCount = static_cast<std::uint32_t>(
        std::clamp(stepped(render, P::OscUnison, frame), 1, static_cast<int>(kAdditiveMaxUnison)));
    state.unisonDetune = render.paramAt(idx(P::OscDetuneCents), frame);
    state.unisonPulseWidth = render.paramAt(idx(P::OscPulseWidth), frame);
    // v1's spread: copies detuned evenly across +-detune/2 and panned across -1..1.
    for (std::uint32_t u = 0; u < kAdditiveMaxUnison; ++u) {
        const std::uint32_t count = state.unisonCount;
        const float spread =
            count > 1 ? static_cast<float>(u) / static_cast<float>(count - 1) : 0.5F;
        const float detune = count > 1 ? (spread - 0.5F) * state.unisonDetune : 0.0F;
        state.unisonRatio[u] = detune == 0.0F ? 1.0F : dsp::centsToRatio(detune);
        state.unisonPan[u] = count > 1 ? (2.0F * spread) - 1.0F : 0.0F;
    }

    // --- formant bank -------------------------------------------------------
    state.formantAmount = std::clamp(render.paramAt(idx(P::FormantAmount), frame), 0.0F, 1.0F);
    if (state.formantAmount > 0.0F) {
        state.formant.configure(
            dsp::VowelSet::Classic3,
            static_cast<std::size_t>(std::clamp(stepped(render, P::FormantVowelA, frame), 0, 4)),
            static_cast<std::size_t>(std::clamp(stepped(render, P::FormantVowelB, frame), 0, 4)),
            std::clamp(render.paramAt(idx(P::FormantMorph), frame), 0.0F, 1.0F), rate);
    }

    // --- the one-pole lowpass and its LFO -------------------------------------
    const float lpCutoff = render.paramAt(idx(P::FilterCutoff), frame);
    state.lpActive = lpCutoff < kFilterBypassHz;
    if (state.lpActive) {
        float cutoff = lpCutoff;
        const float depth = render.paramAt(idx(P::FilterLfoDepth), frame);
        if (depth > 0.0F && render.paramAt(idx(P::FilterLfoRate), frame) > 0.0F) {
            cutoff *= 1.0F + (depth * dsp::sinTurnsF(state.lfoPhase));
        }
        cutoff = std::clamp(cutoff, 20.0F, 20000.0F);
        state.lpCoefficient =
            static_cast<float>(1.0 - dsp::exp(-dsp::kTwoPi * static_cast<double>(cutoff) / rate));
    }

    // --- the resonant filter --------------------------------------------------
    const float resCutoff = render.paramAt(idx(P::ResfilterCutoff), frame);
    state.svfActive = resCutoff < kFilterBypassHz;
    if (state.svfActive) {
        const float envOctaves = render.paramAt(idx(P::ResfilterEnvAmount), frame) *
                                 state.filterEnv.level() * kFilterEnvOctaves;
        const float keyOctaves = render.paramAt(idx(P::ResfilterKeyTrack), frame) *
                                 (static_cast<float>(voice.pitch) - 60.0F) / 12.0F;
        const float cutoff =
            std::clamp(resCutoff * dsp::exp2F(envOctaves + keyOctaves), 20.0F, 20000.0F);
        const float resonance =
            std::clamp(render.paramAt(idx(P::ResfilterResonance), frame), 0.0F, 1.0F);
        // v1's mapping: damping k = 2 - 2 * resonance; 0 self-oscillates.
        state.svfCoefficients = dsp::svfCoefficientsK(cutoff, 2.0 - (2.0 * resonance), rate);
        state.svfType =
            static_cast<std::uint8_t>(std::clamp(stepped(render, P::ResfilterType, frame), 0, 2));
    }

    // --- drive ----------------------------------------------------------------
    const float drive = render.paramAt(idx(P::Drive), frame);
    state.driveGain = drive > 0.0F ? 1.0F + drive : 0.0F;
    state.driveNorm = drive > 0.0F ? dsp::driveNorm(state.driveGain) : 1.0F;
}

void AdditiveInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                                    const graph::VoiceRender& render) noexcept {
    AdditiveVoice& state = stateOf(voice);
    for (std::uint32_t k = 0; k < kAdditiveHarmonics; ++k) {
        state.phase[k] = render.param(idx(P::HarmonicPhase1) + k);
    }
    for (dsp::Oscillator& osc : state.unison) {
        osc.reset(0.0F);
    }
    state.subPhase = 0.0F;
    state.noise.reseed(voiceSeed(voice));
    state.pink = {0.0F, 0.0F, 0.0F};
    state.formant.reset();
    state.lpState = 0.0F;
    state.lfoPhase = 0.0F;
    state.svf.reset();
    state.vibratoPhase = 0.0F;
    state.ampEnv.reset();
    state.filterEnv.reset();
    refresh(voice, state, render, 0);
    state.ampEnv.trigger(state.ampShape, sampleRate());
    state.filterEnv.trigger(state.filterShape, sampleRate());
    voice.level = 0.0F;
}

// NOLINTNEXTLINE(readability-function-size) - one voice's signal path, in v1's order.
bool AdditiveInstrument::renderVoice(graph::Voice& voice, std::span<float> left,
                                     std::span<float> right,
                                     const graph::VoiceRender& render) noexcept {
    AdditiveVoice& state = stateOf(voice);
    const auto rate = static_cast<float>(sampleRate());
    const float inverseRate = 1.0F / rate;

    if (voice.phase == graph::VoicePhase::Released) {
        state.ampEnv.release(state.ampShape, sampleRate());
        state.filterEnv.release(state.filterShape, sampleRate());
    }

    const float velocity = static_cast<float>(voice.velocity) / 127.0F;
    const auto pitch = static_cast<float>(voice.pitch);
    const float dropSemitones = render.param(idx(P::SubDropSemitones));
    const float dropTau = std::max(1.0F, render.param(idx(P::SubDropMs)) * 0.001F * rate);
    const float vibratoRate = render.param(idx(P::VibratoRate));
    const float vibratoDepth = render.param(idx(P::VibratoDepthCents));
    const auto vibratoDelay = static_cast<std::uint64_t>(
        std::max(0.0F, render.param(idx(P::VibratoDelayMs))) * 0.001F * rate);
    const float lfoIncrement = render.param(idx(P::FilterLfoRate)) * inverseRate;
    const int subWave = stepped(render, P::SubWave, 0);
    const int noiseType = stepped(render, P::NoiseType, 0);

    bool alive = true;
    float env = state.ampEnv.level();
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        const std::uint64_t age = voice.age + i;

        // --- pitch: everything in cents, one exp2 -----------------------------
        float cents = i < render.pitchCents.size() ? render.pitchCents[i] : 0.0F;
        if (dropSemitones != 0.0F) {
            cents += 100.0F * dropSemitones *
                     dsp::exp2F(-static_cast<float>(age) / dropTau * std::numbers::log2e_v<float>);
        }
        if (vibratoDepth > 0.0F && vibratoRate > 0.0F) {
            if (age >= vibratoDelay) {
                cents += dsp::sinTurnsF(state.vibratoPhase) * vibratoDepth;
            }
            state.vibratoPhase += vibratoRate * inverseRate;
            if (state.vibratoPhase >= 1.0F) {
                state.vibratoPhase -= 1.0F;
            }
        }
        const float increment = dsp::midiToHz(pitch + (cents * 0.01F)) * inverseRate;

        if (control(voice, i)) {
            refresh(voice, state, render, i);
        }

        // --- sources ------------------------------------------------------------
        float osc = 0.0F;
        for (std::uint32_t k = 0; k < state.topHarmonic; ++k) {
            const float step = increment * state.ratio[k];
            if (step >= 0.5F) {
                continue; // above Nyquist: v1 aliased it; this skips it
            }
            osc += dsp::sinTurnsF(state.phase[k]) * state.amplitude[k];
            float& phase = state.phase[k];
            phase += step;
            if (phase >= 1.0F) {
                phase -= std::floor(phase);
            }
        }

        const float subLevel = render.paramAt(idx(P::SubLevel), i);
        if (subLevel > 0.0F) {
            const float sub = subWave == 1 ? (4.0F * std::abs(state.subPhase - 0.5F)) - 1.0F
                                           : dsp::sinTurnsF(state.subPhase);
            osc += sub * subLevel;
            state.subPhase += increment;
            if (state.subPhase >= 1.0F) {
                state.subPhase -= std::floor(state.subPhase);
            }
        }

        const float noiseLevel = render.paramAt(idx(P::NoiseLevel), i);
        if (noiseLevel > 0.0F) {
            const float white = state.noise.next();
            float noise = white;
            if (noiseType == 1) {
                // Paul Kellett's economy pink filter, v1's exactly.
                state.pink[0] = (0.99765F * state.pink[0]) + (white * 0.0990460F);
                state.pink[1] = (0.96300F * state.pink[1]) + (white * 0.2965164F);
                state.pink[2] = (0.57000F * state.pink[2]) + (white * 1.0526913F);
                noise = (state.pink[0] + state.pink[1] + state.pink[2] + (white * 0.1848F)) * 0.11F;
            }
            osc += noise * noiseLevel;
        }

        float width = 0.0F;
        if (state.unisonWave > 0) {
            const std::uint32_t count = state.unisonCount;
            const float gain = 1.0F / std::sqrt(static_cast<float>(count));
            dsp::OscShape shape = dsp::OscShape::Triangle;
            if (state.unisonWave == 1) {
                shape = dsp::OscShape::Saw;
            } else if (state.unisonWave == 2) {
                shape = dsp::OscShape::Pulse;
            }
            for (std::uint32_t u = 0; u < count; ++u) {
                const float sample =
                    state.unison[u].nextPolyBlep(shape, increment * state.unisonRatio[u],
                                                 state.unisonPulseWidth) *
                    gain;
                osc += sample;
                width += sample * state.unisonPan[u];
            }
        }

        if (state.formantAmount > 0.0F) {
            const float wet = state.formant.process(osc);
            osc += (wet - osc) * state.formantAmount;
        }

        // --- amplitude, filters, drive --------------------------------------------
        env = state.ampEnv.next(state.ampShape);
        float sample = osc * env * velocity;
        const float widthSample = width * env * velocity * kUnisonWidth;
        static_cast<void>(state.filterEnv.next(state.filterShape));

        if (state.lpActive) {
            state.lpState += state.lpCoefficient * (sample - state.lpState);
            sample = state.lpState;
        }
        state.lfoPhase += lfoIncrement;
        if (state.lfoPhase >= 1.0F) {
            state.lfoPhase -= 1.0F;
        }

        if (state.svfActive) {
            const dsp::SvfOutputs y = state.svf.tick(sample, state.svfCoefficients);
            if (state.svfType == 1) {
                sample = y.band;
            } else if (state.svfType == 2) {
                sample = y.high;
            } else {
                sample = y.low;
            }
        }

        if (state.driveGain > 0.0F) {
            sample = dsp::driveTanh(sample, state.driveGain, state.driveNorm);
        }

        left[i] = (sample * 0.5F) - (widthSample * 0.5F);
        right[i] = (sample * 0.5F) + (widthSample * 0.5F);

        if (state.ampEnv.finished()) {
            alive = false;
        }
    }
    voice.level = env * velocity;
    return alive;
}

} // namespace adx::instruments
