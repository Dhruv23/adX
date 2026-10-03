#include "engine/instruments/va/VaInstrument.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::instruments {
namespace {

using P = VaParam;

[[nodiscard]] constexpr std::uint32_t idx(P param) noexcept {
    return static_cast<std::uint32_t>(param);
}

[[nodiscard]] int stepped(const graph::VoiceRender& render, P param, std::uint32_t frame) noexcept {
    return static_cast<int>(std::floor(render.paramAt(idx(param), frame) + 0.5F));
}

[[nodiscard]] dsp::OscShape waveOf(int wave) noexcept {
    return static_cast<dsp::OscShape>(std::clamp(wave, 0, 4));
}

[[nodiscard]] std::uint32_t voiceSeed(const graph::Voice& voice) noexcept {
    // From the voice's identity, never a clock: offline and realtime make the same noise.
    std::uint32_t h = 2166136261U;
    for (const std::uint32_t part : {voice.key.channelId, voice.key.noteId, voice.key.instance}) {
        h = (h ^ part) * 16777619U;
    }
    return h == 0 ? 1U : h;
}

struct LfoRoute {
    VaLfoTarget target;
    float depth;
};

[[nodiscard]] LfoRoute routeOf(const graph::VoiceRender& render, P target, P depth,
                               std::uint32_t frame) noexcept {
    const int t = std::clamp(stepped(render, target, frame), 0, kVaLfoTargetCount - 1);
    return LfoRoute{.target = static_cast<VaLfoTarget>(t),
                    .depth = std::clamp(render.paramAt(idx(depth), frame), 0.0F, 1.0F)};
}

} // namespace

void VaInstrument::prepareInstrument(const graph::PrepareInfo& /*info*/) {
    dsp::Oscillator::prepareTables();
}

float VaInstrument::portamentoSeconds(std::span<const float> params) const noexcept {
    const std::uint32_t at = idx(P::Glide);
    return at < params.size() ? std::max(0.0F, params[at]) : 0.0F;
}

void VaInstrument::refresh(graph::Voice& voice, VaVoice& state, const graph::VoiceRender& render,
                           std::uint32_t frame, float lfoCutoffOctaves) noexcept {
    const auto rate = static_cast<double>(sampleRate());

    // Unison: copies detuned symmetrically about the note, spread across the field.
    const auto count = static_cast<std::uint32_t>(
        std::clamp(stepped(render, P::UnisonVoices, frame), 1, static_cast<int>(kVaMaxUnison)));
    const float detune = render.paramAt(idx(P::UnisonDetune), frame);
    const float spread = std::clamp(render.paramAt(idx(P::UnisonSpread), frame), 0.0F, 1.0F);
    state.unison = count;
    for (std::uint32_t u = 0; u < count; ++u) {
        const float position =
            count > 1 ? ((2.0F * static_cast<float>(u)) / static_cast<float>(count - 1)) - 1.0F
                      : 0.0F;
        state.detune[u] = dsp::centsToRatio(position * detune * 0.5F);
        const float pan = position * spread; // -1 .. 1
        state.panLeft[u] = dsp::cosTurnsF((pan + 1.0F) * 0.125F);
        state.panRight[u] = dsp::sinTurnsF((pan + 1.0F) * 0.125F);
    }
    // Equal-power stacking: N uncorrelated copies are sqrt(N) louder, not N.
    state.unisonNorm = 1.0F / std::sqrt(static_cast<float>(count));

    state.ampShape.attackSeconds = render.paramAt(idx(P::EnvAttack), frame);
    state.ampShape.decaySeconds = render.paramAt(idx(P::EnvDecay), frame);
    state.ampShape.sustain = render.paramAt(idx(P::EnvSustain), frame);
    state.ampShape.releaseSeconds = render.paramAt(idx(P::EnvRelease), frame);
    state.filterShape.attackSeconds = render.paramAt(idx(P::FilterEnvAttack), frame);
    state.filterShape.decaySeconds = render.paramAt(idx(P::FilterEnvDecay), frame);
    state.filterShape.sustain = render.paramAt(idx(P::FilterEnvSustain), frame);
    state.filterShape.releaseSeconds = render.paramAt(idx(P::FilterEnvRelease), frame);

    // The filter: cutoff moved in octaves by its envelope, the key, the velocity and an
    // LFO, then coefficients for whichever topology is selected.
    const float octaves =
        (render.paramAt(idx(P::FilterEnvAmount), frame) * state.filterEnv.level()) +
        (render.paramAt(idx(P::FilterKeyTrack), frame) * (static_cast<float>(voice.pitch) - 60.0F) /
         12.0F) +
        (render.paramAt(idx(P::FilterVelocity), frame) * (state.velocity - 1.0F)) +
        lfoCutoffOctaves;
    const float cutoff = std::clamp(
        render.paramAt(idx(P::FilterCutoff), frame) * dsp::exp2F(octaves), 20.0F, 20000.0F);
    const float resonance = std::clamp(render.paramAt(idx(P::FilterResonance), frame), 0.0F, 1.0F);
    state.filterType = static_cast<VaFilterType>(
        std::clamp(stepped(render, P::FilterType, frame), 0, kVaFilterTypeCount - 1));
    state.filterDrive = std::max(0.0F, render.paramAt(idx(P::FilterDrive), frame));
    if (state.filterType == VaFilterType::Ladder) {
        state.ladderCoefficients = dsp::ladderCoefficients(cutoff, resonance, rate);
    } else {
        // Q from 0.5 (no resonance) to 20.
        state.svfCoefficients =
            dsp::svfCoefficients(cutoff, 0.5 + (19.5 * resonance * resonance), rate);
    }
}

void VaInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                              const graph::VoiceRender& render) noexcept {
    VaVoice& state = stateOf(voice);
    // Unison copies start at spread phases, as analogue oscillators would not be in
    // step; a fixed spread, so the same note sounds the same every time.
    for (std::uint32_t u = 0; u < kVaMaxUnison; ++u) {
        const float phase = static_cast<float>(u) * 0.618034F;
        state.osc1[u].reset(phase - std::floor(phase));
        state.osc2[u].reset((phase * 0.5F) - std::floor(phase * 0.5F));
    }
    state.sub.reset(0.0F);
    state.noise.reseed(voiceSeed(voice));
    for (auto& filter : state.ladder) {
        filter.reset();
    }
    for (auto& filter : state.svf) {
        filter.reset();
    }
    state.velocity = static_cast<float>(voice.velocity) / 127.0F;
    state.released = false;

    // An LFO in free mode keeps the channel's time, so two notes see one LFO; retrig
    // starts it from zero at the note. The channel's clock, never the wall clock.
    const double seconds = static_cast<double>(pool().clock()) / static_cast<double>(sampleRate());
    for (std::uint32_t l = 0; l < 2; ++l) {
        const P rateParam = l == 0 ? P::Lfo1Rate : P::Lfo2Rate;
        const P retrig = l == 0 ? P::Lfo1Retrig : P::Lfo2Retrig;
        state.lfo[l] = dsp::Lfo{voiceSeed(voice) ^ (l + 1)};
        if (render.param(idx(retrig)) >= 0.5F) {
            state.lfo[l].reset(0.0F);
        } else {
            const double cycles = seconds * static_cast<double>(render.param(idx(rateParam)));
            state.lfo[l].reset(static_cast<float>(cycles - std::floor(cycles)));
        }
    }
    state.ampEnv.reset();
    state.filterEnv.reset();
    refresh(voice, state, render, 0, 0.0F);
    state.ampEnv.trigger(state.ampShape, sampleRate());
    state.filterEnv.trigger(state.filterShape, sampleRate());
    voice.level = 0.0F;
}

// NOLINTNEXTLINE(readability-function-size) - one voice: modulation, sources, filter, amp.
bool VaInstrument::renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                               const graph::VoiceRender& render) noexcept {
    VaVoice& state = stateOf(voice);
    if (voice.phase == graph::VoicePhase::Released && !state.released) {
        state.released = true;
        state.ampEnv.release(state.ampShape, sampleRate());
        state.filterEnv.release(state.filterShape, sampleRate());
    }
    const float inverseRate = 1.0F / static_cast<float>(sampleRate());
    const auto pitch = static_cast<float>(voice.pitch);
    const dsp::OscShape wave1 = waveOf(stepped(render, P::Osc1Wave, 0));
    const dsp::OscShape wave2 = waveOf(stepped(render, P::Osc2Wave, 0));
    const float offset1 = (12.0F * static_cast<float>(stepped(render, P::Osc1Octave, 0))) +
                          static_cast<float>(stepped(render, P::Osc1Semi, 0)) +
                          (render.param(idx(P::Osc1Fine)) * 0.01F);
    const float offset2 = (12.0F * static_cast<float>(stepped(render, P::Osc2Octave, 0))) +
                          static_cast<float>(stepped(render, P::Osc2Semi, 0)) +
                          (render.param(idx(P::Osc2Fine)) * 0.01F);
    const std::array<float, 2> lfoRate{render.param(idx(P::Lfo1Rate)) * inverseRate,
                                       render.param(idx(P::Lfo2Rate)) * inverseRate};
    const std::array<dsp::LfoShape, 2> lfoShape{
        static_cast<dsp::LfoShape>(
            std::clamp(stepped(render, P::Lfo1Shape, 0), 0, dsp::kLfoShapeCount - 1)),
        static_cast<dsp::LfoShape>(
            std::clamp(stepped(render, P::Lfo2Shape, 0), 0, dsp::kLfoShapeCount - 1))};

    const float level = dsp::dbToGainF(render.param(idx(P::Level)));
    const bool levelAutomated =
        idx(P::Level) < render.automation.size() && render.automation[idx(P::Level)] != nullptr;

    bool alive = true;
    float env = state.ampEnv.level();
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        // --- modulation ---------------------------------------------------------
        float pitchMod = 0.0F;
        float cutoffMod = 0.0F;
        float ampMod = 1.0F;
        float panMod = 0.0F;
        float widthMod = 0.0F;
        const std::array<LfoRoute, 2> routes{routeOf(render, P::Lfo1Target, P::Lfo1Depth, i),
                                             routeOf(render, P::Lfo2Target, P::Lfo2Depth, i)};
        for (std::uint32_t l = 0; l < 2; ++l) {
            const float v = state.lfo[l].next(lfoShape[l], lfoRate[l]) * routes[l].depth;
            switch (routes[l].target) {
            case VaLfoTarget::Pitch:
                pitchMod += 12.0F * v;
                break;
            case VaLfoTarget::Cutoff:
                cutoffMod += 4.0F * v;
                break;
            case VaLfoTarget::Amplitude:
                ampMod *= 1.0F - (routes[l].depth * 0.5F) + (0.5F * v);
                break;
            case VaLfoTarget::Pan:
                panMod += v;
                break;
            case VaLfoTarget::PulseWidth:
                widthMod += 0.45F * v;
                break;
            case VaLfoTarget::None:
                break;
            }
        }
        if (control(voice, i)) {
            refresh(voice, state, render, i, cutoffMod);
        }

        // --- sources --------------------------------------------------------------
        const float cents = i < render.pitchCents.size() ? render.pitchCents[i] : 0.0F;
        const float note = pitch + (cents * 0.01F) + pitchMod;
        const float base1 = dsp::midiToHz(note + offset1) * inverseRate;
        const float base2 = dsp::midiToHz(note + offset2) * inverseRate;
        const float level1 = render.paramAt(idx(P::Osc1Level), i);
        const float level2 = render.paramAt(idx(P::Osc2Level), i);
        const float width1 =
            std::clamp(render.paramAt(idx(P::Osc1PulseWidth), i) + widthMod, 0.05F, 0.95F);
        const float width2 =
            std::clamp(render.paramAt(idx(P::Osc2PulseWidth), i) + widthMod, 0.05F, 0.95F);
        float l = 0.0F;
        float r = 0.0F;
        for (std::uint32_t u = 0; u < state.unison; ++u) {
            float s = 0.0F;
            if (level1 > 0.0F) {
                s += state.osc1[u].nextBandLimited(wave1, base1 * state.detune[u], width1) * level1;
            }
            if (level2 > 0.0F) {
                s += state.osc2[u].nextBandLimited(wave2, base2 * state.detune[u], width2) * level2;
            }
            l += s * state.panLeft[u];
            r += s * state.panRight[u];
        }
        l *= state.unisonNorm;
        r *= state.unisonNorm;
        const float subLevel = render.paramAt(idx(P::SubLevel), i);
        const float noiseLevel = render.paramAt(idx(P::NoiseLevel), i);
        if (subLevel > 0.0F) {
            const float sub =
                state.sub.nextBandLimited(dsp::OscShape::Sine, base1 * 0.5F) * subLevel;
            l += sub * 0.70710678F;
            r += sub * 0.70710678F;
        }
        if (noiseLevel > 0.0F) {
            const float n = state.noise.next() * noiseLevel * 0.70710678F;
            l += n;
            r += n;
        }

        // --- filter ---------------------------------------------------------------
        static_cast<void>(state.filterEnv.next(state.filterShape));
        if (state.filterType == VaFilterType::Ladder) {
            l = state.ladder[0].process(l, state.ladderCoefficients, state.filterDrive);
            r = state.ladder[1].process(r, state.ladderCoefficients, state.filterDrive);
        } else {
            dsp::SvfMode mode = dsp::SvfMode::Notch;
            switch (state.filterType) {
            case VaFilterType::LowPass:
                mode = dsp::SvfMode::LowPass;
                break;
            case VaFilterType::BandPass:
                mode = dsp::SvfMode::BandPass;
                break;
            case VaFilterType::HighPass:
                mode = dsp::SvfMode::HighPass;
                break;
            default:
                break;
            }
            if (state.filterDrive > 0.0F) {
                const float gain = 1.0F + state.filterDrive;
                l = dsp::tanhF(l * gain) / gain;
                r = dsp::tanhF(r * gain) / gain;
            }
            l = state.svf[0].process(l, state.svfCoefficients, mode);
            r = state.svf[1].process(r, state.svfCoefficients, mode);
        }

        // --- amplitude ------------------------------------------------------------
        env = state.ampEnv.next(state.ampShape);
        const float gain =
            env * state.velocity * ampMod *
            (levelAutomated ? dsp::dbToGainF(render.paramAt(idx(P::Level), i)) : level);
        if (panMod != 0.0F) {
            const float p = std::clamp(panMod, -1.0F, 1.0F);
            l *= p > 0.0F ? 1.0F - p : 1.0F;
            r *= p < 0.0F ? 1.0F + p : 1.0F;
        }
        left[i] = l * gain;
        right[i] = r * gain;
        if (state.ampEnv.finished()) {
            alive = false;
        }
    }
    voice.level = env * state.velocity;
    return alive;
}

} // namespace adx::instruments
