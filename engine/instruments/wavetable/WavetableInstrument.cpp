#include "engine/instruments/wavetable/WavetableInstrument.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

namespace adx::instruments {
namespace {

using P = WavetableParam;

[[nodiscard]] constexpr std::uint32_t idx(P param) noexcept {
    return static_cast<std::uint32_t>(param);
}

[[nodiscard]] int stepped(const graph::VoiceRender& render, P param, std::uint32_t frame) noexcept {
    return static_cast<int>(dsp::floorExact(render.paramAt(idx(param), frame) + 0.5F));
}

[[nodiscard]] std::uint32_t voiceSeed(const graph::Voice& voice) noexcept {
    std::uint32_t h = 2166136261U;
    for (const std::uint32_t part : {voice.key.channelId, voice.key.noteId, voice.key.instance}) {
        h = (h ^ part) * 16777619U;
    }
    return h == 0 ? 1U : h;
}

/// The table read at `position` in [0, 1] across its frames: two adjacent frames,
/// crossfaded. For the spectral table the frames are already the interpolation, so
/// the crossfade is between neighbours a quarter of a source step apart.
[[nodiscard]] float readTable(const dsp::WaveTable& table, float position, std::size_t level,
                              float phase) noexcept {
    const std::size_t frames = table.frames();
    if (frames <= 1) {
        return table.read(0, level, phase);
    }
    const float at = std::clamp(position, 0.0F, 1.0F) * static_cast<float>(frames - 1);
    const auto frame = std::min(static_cast<std::size_t>(at), frames - 2);
    const float t = at - static_cast<float>(frame);
    const float a = table.read(frame, level, phase);
    const float b = table.read(frame + 1, level, phase);
    return a + ((b - a) * t);
}

} // namespace

WavetableInstrument::~WavetableInstrument() {
    destroyWavetablePins(m_pins);
}

void WavetableInstrument::setUserTable(const WavetablePair* table, WavetablePins* pins) noexcept {
    m_user = table;
    destroyWavetablePins(m_pins);
    m_pins = pins;
}

void WavetableInstrument::prepareInstrument(const graph::PrepareInfo& /*info*/) {
    for (std::size_t b = 0; b + 1 < kWavetableBankCount; ++b) {
        m_tables[b] = &builtinWavetable(static_cast<WavetableBank>(b));
    }
    // Without a table of its own, the User bank plays the Classic one.
    m_tables[kWavetableBankCount - 1] = m_user != nullptr ? m_user : m_tables[0];
}

float WavetableInstrument::portamentoSeconds(std::span<const float> params) const noexcept {
    const std::uint32_t at = idx(P::Glide);
    return at < params.size() ? std::max(0.0F, params[at]) : 0.0F;
}

std::uint8_t WavetableInstrument::voiceMode(std::span<const float> params) const noexcept {
    return static_cast<std::uint8_t>(modVoiceMode(params, kWavetableMod));
}

bool WavetableInstrument::glideAlways(std::span<const float> params) const noexcept {
    return modGlideAlways(params, kWavetableMod);
}

void WavetableInstrument::retriggerVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                                         const graph::VoiceRender& render) noexcept {
    WavetableVoice& state = stateOf(voice);
    state.velocity = static_cast<float>(voice.velocity) / 127.0F;
    state.released = false;
    state.env.trigger(state.shape, sampleRate());
    modRetrigger(state.mod, render, kWavetableMod);
}

void WavetableInstrument::refresh(WavetableVoice& state, const graph::VoiceRender& render,
                                  std::uint32_t frame, const dsp::ModFrame& mod) noexcept {
    const auto count = static_cast<std::uint32_t>(std::clamp(
        stepped(render, P::UnisonVoices, frame), 1, static_cast<int>(kWavetableMaxUnison)));
    const float detune = std::max(0.0F, render.paramAt(idx(P::UnisonDetune), frame) +
                                            mod.of(dsp::ModDest::UnisonDetune));
    const float spread = std::clamp(render.paramAt(idx(P::UnisonSpread), frame), 0.0F, 1.0F);
    state.unison = count;
    for (std::uint32_t u = 0; u < count; ++u) {
        const float position =
            count > 1 ? ((2.0F * static_cast<float>(u)) / static_cast<float>(count - 1)) - 1.0F
                      : 0.0F;
        state.detune[u] = dsp::centsToRatio(position * detune * 0.5F);
        const float pan = position * spread;
        state.panLeft[u] = dsp::cosTurnsF((pan + 1.0F) * 0.125F);
        state.panRight[u] = dsp::sinTurnsF((pan + 1.0F) * 0.125F);
    }
    state.unisonNorm = 1.0F / std::sqrt(static_cast<float>(count));
    state.shape.attackSeconds = render.paramAt(idx(P::EnvAttack), frame);
    state.shape.decaySeconds = render.paramAt(idx(P::EnvDecay), frame);
    state.shape.sustain = render.paramAt(idx(P::EnvSustain), frame);
    state.shape.releaseSeconds = render.paramAt(idx(P::EnvRelease), frame);
    const float octaves = mod.of(dsp::ModDest::Cutoff);
    const float cutoff = std::clamp(render.paramAt(idx(P::FilterCutoff), frame) *
                                        (octaves != 0.0F ? dsp::exp2F(octaves) : 1.0F),
                                    20.0F, 20000.0F);
    const float resonance =
        std::clamp(render.paramAt(idx(P::FilterResonance), frame) + mod.of(dsp::ModDest::Resonance),
                   0.0F, 1.0F);
    state.coefficients = dsp::svfCoefficients(
        std::min(static_cast<double>(cutoff), sampleRate() * 0.45),
        0.5 + (19.5 * resonance * resonance), static_cast<double>(sampleRate()));
}

void WavetableInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                                     const graph::VoiceRender& render) noexcept {
    WavetableVoice& state = stateOf(voice);
    for (std::uint32_t u = 0; u < kWavetableMaxUnison; ++u) {
        const float phase = static_cast<float>(u) * 0.618034F;
        state.phase[u] = phase - dsp::floorExact(phase);
    }
    for (auto& filter : state.filter) {
        filter.reset();
    }
    state.lfo = dsp::Lfo{voiceSeed(voice)};
    state.lfo.reset(0.0F);
    state.velocity = static_cast<float>(voice.velocity) / 127.0F;
    state.released = false;
    modStart(state.mod, voice, render, kWavetableMod, modVoiceSeed(voice),
             static_cast<double>(pool().clock()) / static_cast<double>(sampleRate()));
    state.env.reset();
    refresh(state, render, 0, dsp::neutralModFrame());
    state.env.trigger(state.shape, sampleRate());
    voice.level = 0.0F;
}

// NOLINTNEXTLINE(readability-function-size) - one voice: position, unison, filter, amp.
bool WavetableInstrument::renderVoice(graph::Voice& voice, std::span<float> left,
                                      std::span<float> right,
                                      const graph::VoiceRender& render) noexcept {
    WavetableVoice& state = stateOf(voice);
    if (voice.phase == graph::VoicePhase::Released && !state.released) {
        state.released = true;
        state.env.release(state.shape, sampleRate());
    }
    modBegin(state.mod, voice, render, kWavetableMod);
    dsp::ModFrame scratch;
    const auto bank = static_cast<std::size_t>(
        std::clamp(stepped(render, P::Table, 0), 0, kWavetableBankCount - 1));
    const WavetablePair& pair = *m_tables[bank];
    const bool spectral = stepped(render, P::Morph, 0) >= 1;
    const dsp::WaveTable& table = spectral ? pair.spectral : pair.linear;
    const float inverseRate = 1.0F / static_cast<float>(sampleRate());
    const float lfoIncrement = render.param(idx(P::LfoRate)) * inverseRate;
    const float lfoDepth = std::clamp(render.param(idx(P::LfoDepth)), 0.0F, 1.0F);
    const float level = dsp::dbToGainF(render.param(idx(P::Level)));
    const bool levelAutomated =
        idx(P::Level) < render.automation.size() && render.automation[idx(P::Level)] != nullptr;
    const auto pitch = static_cast<float>(voice.pitch);

    bool alive = true;
    float env = state.env.level();
    dsp::MidiHz hzOf; // the pitch's Hz, again only when it moves
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        const bool onGrid = control(voice, i);
        const dsp::ModFrame& mod =
            modTick(state.mod, voice, render, kWavetableMod, i, onGrid, scratch);
        if (onGrid) {
            refresh(state, render, i, mod);
        }
        const float lfo = state.lfo.next(dsp::LfoShape::Triangle, lfoIncrement);
        const float position = render.paramAt(idx(P::Position), i) + (0.5F * lfoDepth * lfo) +
                               mod.of(dsp::ModDest::WtPosition);
        const float cents = i < render.pitchCents.size() ? render.pitchCents[i] : 0.0F;
        const float base =
            hzOf(pitch + (cents * 0.01F) + mod.of(dsp::ModDest::Pitch)) * inverseRate;
        float l = 0.0F;
        float r = 0.0F;
        for (std::uint32_t u = 0; u < state.unison; ++u) {
            const float increment = base * state.detune[u];
            const float s =
                readTable(table, position, dsp::waveTableLevel(increment), state.phase[u]);
            state.phase[u] += increment;
            state.phase[u] -= dsp::floorExact(state.phase[u]);
            l += s * state.panLeft[u];
            r += s * state.panRight[u];
        }
        l = state.filter[0].process(l * state.unisonNorm, state.coefficients,
                                    dsp::SvfMode::LowPass);
        r = state.filter[1].process(r * state.unisonNorm, state.coefficients,
                                    dsp::SvfMode::LowPass);
        env = state.env.next(state.shape);
        const float gain =
            env * state.velocity *
            (levelAutomated ? dsp::dbToGainF(render.paramAt(idx(P::Level), i)) : level);
        left[i] = l * gain;
        right[i] = r * gain;
        if (&mod == &scratch) {
            left[i] *= mod.of(dsp::ModDest::Level);
            right[i] *= mod.of(dsp::ModDest::Level);
            modPan(mod.of(dsp::ModDest::Pan), left[i], right[i]);
        }
        if (state.env.finished()) {
            alive = false;
        }
    }
    voice.level = env * state.velocity;
    return alive;
}

} // namespace adx::instruments
