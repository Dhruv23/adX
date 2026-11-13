#include "engine/instruments/gm/GmInstrument.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "engine/dsp/Math.h"

namespace adx::instruments {
namespace {

using format::soundfont::BankEnvelope;
using format::soundfont::BankLoop;
using format::soundfont::BankZone;
using P = GmParam;

constexpr std::uint8_t kDelay = 0;
constexpr std::uint8_t kAttack = 1;
constexpr std::uint8_t kHold = 2;
constexpr std::uint8_t kDecay = 3;
constexpr std::uint8_t kSustain = 4;
constexpr std::uint8_t kRelease = 5;
constexpr std::uint8_t kDone = 6;
/// A volume envelope's floor: 96 dB down is silence, as SF2 defines its times.
constexpr float kFloor = 1.5848932e-5F;
constexpr float kChokeSeconds = 0.002F;
/// SF2 2.04 8.4.4: CC1 to vibrato LFO pitch depth, 50 cents at full.
constexpr float kWheelVibratoCents = 50.0F;

[[nodiscard]] constexpr std::uint32_t idx(P p) noexcept {
    return static_cast<std::uint32_t>(p);
}

[[nodiscard]] int choice(const graph::VoiceRender& render, P p, int highest) noexcept {
    return std::clamp(static_cast<int>(dsp::floorExact(render.param(idx(p)) + 0.5F)), 0, highest);
}

[[nodiscard]] int partChoice(const graph::VoiceRender& render, std::uint32_t part,
                             GmPartParam which) noexcept {
    return std::clamp(
        static_cast<int>(dsp::floorExact(render.param(gmPartParam(part, which)) + 0.5F)), 0, 127);
}

/// A part's level and balance now: GM's square law on volume and expression, unity at
/// volume 100 and expression 127; an equal-power balance, unity at the centre.
struct PartMix {
    float left;
    float right;
};
[[nodiscard]] PartMix partMix(float volumeCc, float expressionCc, float panValue) noexcept {
    const float volume = std::clamp(volumeCc, 0.0F, 127.0F) / 100.0F;
    const float expression = std::clamp(expressionCc, 0.0F, 127.0F) / 127.0F;
    const float level = volume * volume * expression * expression;
    const float pan = std::clamp(panValue, -1.0F, 1.0F);
    if (pan == 0.0F) {
        return PartMix{.left = level, .right = level};
    }
    return PartMix{
        .left = level * dsp::cosTurnsF((pan + 1.0F) * 0.125F) * std::numbers::sqrt2_v<float>,
        .right = level * dsp::sinTurnsF((pan + 1.0F) * 0.125F) * std::numbers::sqrt2_v<float>};
}

[[nodiscard]] std::uint32_t frames(float seconds, float rate) noexcept {
    return static_cast<std::uint32_t>(std::max(0.0F, seconds) * rate);
}

/// The per-sample factor that falls 96 dB in `seconds`.
[[nodiscard]] float fallFactor(float seconds, float rate) noexcept {
    const float n = std::max(1.0F, seconds * rate);
    return dsp::exp2F(-96.0F / 6.0206F / n);
}

[[nodiscard]] float keyScale(float timecentsPerKey, std::uint8_t key) noexcept {
    if (timecentsPerKey == 0.0F) {
        return 1.0F;
    }
    return dsp::exp2F(-timecentsPerKey * (static_cast<float>(key) - 60.0F) / 1200.0F);
}

void enterOne(GmEnvelope& env, std::uint8_t stage, const BankEnvelope& shape, std::uint8_t key,
              float rate, bool volume) noexcept {
    env.stage = stage;
    switch (stage) {
    case kDelay:
        env.left = frames(shape.delay, rate);
        env.level = 0.0F;
        break;
    case kAttack:
        env.left = frames(shape.attack, rate);
        env.step = env.left > 0 ? (1.0F - env.level) / static_cast<float>(env.left) : 0.0F;
        break;
    case kHold:
        env.level = 1.0F;
        env.left = frames(shape.hold * keyScale(shape.keyToHold, key), rate);
        break;
    case kDecay: {
        const float seconds = shape.decay * keyScale(shape.keyToDecay, key);
        env.left = frames(seconds, rate);
        if (volume) {
            env.step = fallFactor(seconds, rate);
        } else {
            env.step = env.left > 0 ? 1.0F / static_cast<float>(env.left) : 1.0F;
        }
        break;
    }
    case kRelease:
        env.left = frames(shape.release, rate);
        if (volume) {
            env.step = fallFactor(shape.release, rate);
        } else {
            env.step = env.left > 0 ? env.level / static_cast<float>(env.left) : 1.0F;
        }
        break;
    default:
        break;
    }
}

/// Enters `stage`, then passes straight through any empty delay, attack or hold, so a
/// zone with none starts at full level on its first sample (zone_articulation_inert).
void enter(GmEnvelope& env, std::uint8_t stage, const BankEnvelope& shape, std::uint8_t key,
           float rate, bool volume) noexcept {
    enterOne(env, stage, shape, key, rate, volume);
    while (env.left == 0 && env.stage < kDecay) {
        enterOne(env, static_cast<std::uint8_t>(env.stage + 1), shape, key, rate, volume);
    }
}

/// The envelope's level now, then one sample on.
float next(GmEnvelope& env, const BankEnvelope& shape, std::uint8_t key, float rate,
           bool volume) noexcept {
    const float out = env.level;
    switch (env.stage) {
    case kDelay:
        if (env.left == 0) {
            enter(env, kAttack, shape, key, rate, volume);
        } else {
            --env.left;
        }
        break;
    case kAttack:
        if (env.left == 0) {
            enter(env, kHold, shape, key, rate, volume);
        } else {
            env.level = std::min(1.0F, env.level + env.step);
            --env.left;
        }
        break;
    case kHold:
        if (env.left == 0) {
            enter(env, kDecay, shape, key, rate, volume);
        } else {
            --env.left;
        }
        break;
    case kDecay:
        env.level = volume ? env.level * env.step : env.level - env.step;
        if (env.level <= shape.sustain) {
            env.level = shape.sustain;
            env.stage = kSustain;
        }
        break;
    case kSustain:
        break;
    case kRelease:
        env.level = volume ? env.level * env.step : std::max(0.0F, env.level - env.step);
        if ((volume && env.level < kFloor) || (!volume && env.level <= 0.0F)) {
            env.level = 0.0F;
            env.stage = kDone;
        }
        break;
    default:
        env.level = 0.0F;
        break;
    }
    return out;
}

/// The triangle SF2's LFOs are: -1..1, starting at 0 rising.
[[nodiscard]] float triangle(float phase) noexcept {
    const float p = phase - dsp::floorExact(phase);
    if (p < 0.25F) {
        return 4.0F * p;
    }
    return p < 0.75F ? 2.0F - (4.0F * p) : (4.0F * p) - 4.0F;
}

} // namespace

GmInstrument::~GmInstrument() {
    destroyGmPins(m_pins);
}

void GmInstrument::setBank(const GmBankView& view, GmPins* pins) noexcept {
    m_view = view;
    destroyGmPins(m_pins);
    m_pins = pins;
}

const GmPresetView* GmInstrument::find(std::uint16_t bank, std::uint8_t program) const noexcept {
    for (const GmPresetView& p : m_view.presets) {
        if (p.bank == bank && p.program == program) {
            return &p;
        }
    }
    // An unknown program: the same one on bank 0. A missing kit: the first kit.
    for (const GmPresetView& p : m_view.presets) {
        if (bank == kGmDrumBank ? p.bank == kGmDrumBank : (p.bank == 0 && p.program == program)) {
            return &p;
        }
    }
    return bank == kGmDrumBank || m_view.presets.empty() ? nullptr : &m_view.presets.front();
}

void GmInstrument::refreshLayer(GmLayer& layer, float cents) noexcept {
    layer.increment =
        static_cast<float>(layer.rateRatio * static_cast<double>(dsp::exp2F(cents / 1200.0F)));
}

void GmInstrument::startLayer(GmLayer& layer, const BankZone& zone, const graph::Voice& voice,
                              float velocity, float tune) const noexcept {
    const auto rate = static_cast<float>(sampleRate());
    layer.zone = &zone;
    layer.sample = m_view.samples[zone.sample];
    layer.position = static_cast<double>(zone.start);
    layer.rateRatio = static_cast<double>(layer.sample.rate) / static_cast<double>(sampleRate());
    layer.baseCents =
        ((static_cast<float>(voice.pitch) - static_cast<float>(zone.rootKey)) * zone.scaleTuning) +
        zone.tune + tune;
    refreshLayer(layer, layer.baseCents);
    // Attenuation, and SF2's default velocity curve: GM's (velocity / 127)^2.
    const float db =
        -zone.attenuation +
        (velocity * 40.0F *
         dsp::log2F(std::max(1.0F, static_cast<float>(voice.velocity)) / 127.0F) * 0.30103F);
    const float gain = dsp::dbToGainF(db);
    const float pan = std::clamp(zone.pan, -1.0F, 1.0F);
    layer.gainLeft = gain * dsp::cosTurnsF((pan + 1.0F) * 0.125F) * std::numbers::sqrt2_v<float>;
    layer.gainRight = gain * dsp::sinTurnsF((pan + 1.0F) * 0.125F) * std::numbers::sqrt2_v<float>;
    layer.volume = GmEnvelope{.stage = kDelay, .level = 0.0F, .left = 0, .step = 0.0F};
    layer.modulation = layer.volume;
    enter(layer.volume, kDelay, zone.volume, voice.pitch, rate, true);
    enter(layer.modulation, kDelay, zone.modulation, voice.pitch, rate, false);
    layer.vibPhase = 0.0F;
    layer.modPhase = 0.0F;
    layer.vibDelay = frames(zone.vibrato.delay, rate);
    layer.modDelay = frames(zone.modLfo.delay, rate);
    // The filter: SF2's default velocity-to-cutoff modulator, up to 2400 cents down.
    float cutoff = zone.filterHz;
    if (cutoff < 19500.0F || zone.modToFilter != 0.0F || zone.modLfo.toFilter != 0.0F) {
        cutoff *= dsp::exp2F(-2.0F * (1.0F - (static_cast<float>(voice.velocity) / 127.0F)));
        layer.filtered = true;
        const double q = 0.70710678 * dsp::dbToGain(static_cast<double>(zone.filterQ));
        layer.coefficients =
            dsp::svfCoefficients(std::clamp(static_cast<double>(cutoff), 20.0, 0.45 * sampleRate()),
                                 q, static_cast<double>(sampleRate()));
        for (dsp::SvFilter& f : layer.filter) {
            f.reset();
        }
    } else {
        layer.filtered = false;
    }
    layer.active = layer.sample.frames > 0 && zone.end > zone.start;
}

void GmInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& event,
                              const graph::VoiceRender& render) noexcept {
    GmVoice& state = stateOf(voice);
    state.layerCount = 0;
    state.released = false;
    state.cut = false;
    state.exclusiveClass = 0;
    state.velocity = std::clamp(render.param(idx(P::Velocity)), 0.0F, 1.0F);
    voice.level = 0.0F;
    const bool parts = render.param(idx(P::Part)) >= 0.5F;
    state.part = parts ? static_cast<std::uint8_t>(std::min<std::uint32_t>(event.part, 15)) : 0;
    bool drums = false;
    std::uint16_t bank = 0;
    std::uint8_t program = 0;
    const Live& live = m_live[state.part];
    if (parts) {
        drums = render.param(gmPartParam(state.part, GmPartParam::Drums)) >= 0.5F;
        bank = static_cast<std::uint16_t>(
            drums ? kGmDrumBank
                  : (live.bank >= 0 ? live.bank
                                    : partChoice(render, state.part, GmPartParam::BankMsb)) *
                        128);
        program = static_cast<std::uint8_t>(
            live.program >= 0 ? live.program
                              : partChoice(render, state.part, GmPartParam::Program));
    } else {
        drums = render.param(idx(P::Drums)) >= 0.5F;
        if (drums) {
            bank = kGmDrumBank;
        } else if (live.bank >= 0) {
            bank = static_cast<std::uint16_t>(live.bank * 128);
        } else {
            bank = static_cast<std::uint16_t>((choice(render, P::BankMsb, 127) * 128) +
                                              choice(render, P::BankLsb, 127));
        }
        program = static_cast<std::uint8_t>(live.program >= 0 ? live.program
                                                              : choice(render, P::Program, 127));
    }
    const GmPresetView* preset = find(bank, program);
    if (preset == nullptr) {
        return;
    }
    const float tune = render.param(idx(P::Tune));
    for (const BankZone& zone : m_view.zones.subspan(preset->firstZone, preset->zoneCount)) {
        if (state.layerCount >= kGmLayers) {
            break;
        }
        if (!zone.matches(voice.pitch, voice.velocity) || zone.sample >= m_view.samples.size()) {
            continue;
        }
        startLayer(state.layers[state.layerCount], zone, voice, state.velocity, tune);
        ++state.layerCount;
        state.exclusiveClass = std::max(state.exclusiveClass, zone.exclusiveClass);
    }
    if (state.exclusiveClass != 0) {
        // The rest of this class are cut: a closed hat stops the open one.
        for (graph::Voice& other : pool().voices()) {
            if (&other != &voice && other.phase != graph::VoicePhase::Free) {
                GmVoice& them = stateOf(other);
                if (them.exclusiveClass == state.exclusiveClass && them.part == state.part) {
                    them.cut = true;
                }
            }
        }
    }
}

void GmInstrument::controller(const graph::BlockEvent& event,
                              const graph::VoiceRender& render) noexcept {
    const bool parts = render.param(idx(P::Part)) >= 0.5F;
    const std::uint8_t part = parts ? static_cast<std::uint8_t>(event.part & 0x0FU) : 0;
    Live& live = m_live[part];
    const auto value = static_cast<std::int16_t>(std::min<int>(event.velocity, 127));
    if (event.pitch == graph::kProgramChange) {
        live.program = value;
        return;
    }
    const auto forEachVoice = [&](auto&& fn) {
        for (graph::Voice& voice : pool().voices()) {
            if (voice.phase != graph::VoicePhase::Free && stateOf(voice).part == part) {
                fn(voice);
            }
        }
    };
    switch (event.pitch) {
    case 0:
        live.bank = value;
        break;
    case 1:
        live.wheel = value;
        break;
    case 7:
        live.volume = value;
        break;
    case 10:
        live.pan = std::clamp((static_cast<float>(value) - 64.0F) / 63.0F, -1.0F, 1.0F);
        live.panSet = true;
        break;
    case 11:
        live.expression = value;
        break;
    case 120: // All Sound Off: cut, as an exclusive class does
        forEachVoice([&](graph::Voice& voice) { stateOf(voice).cut = true; });
        break;
    case 121: // Reset All Controllers: the wheel and expression, not volume or pan (GM)
        live.wheel = -1;
        live.expression = -1;
        break;
    case 123: // All Notes Off: a release
        forEachVoice([](graph::Voice& voice) { graph::VoicePool::release(voice); });
        break;
    default:
        break;
    }
}

// NOLINTNEXTLINE(readability-function-size) - layers: envelopes, LFOs, read, filter.
bool GmInstrument::renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                               const graph::VoiceRender& render) noexcept {
    GmVoice& state = stateOf(voice);
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);
    if (state.layerCount == 0) {
        voice.level = 0.0F;
        return false;
    }
    const auto rate = static_cast<float>(sampleRate());
    if ((voice.phase == graph::VoicePhase::Released && !state.released) || state.cut) {
        state.released = true;
        for (std::uint32_t l = 0; l < state.layerCount; ++l) {
            GmLayer& layer = state.layers[l];
            if (layer.volume.stage < kRelease) {
                BankEnvelope shape = layer.zone->volume;
                if (state.cut) {
                    shape.release = kChokeSeconds;
                }
                enter(layer.volume, kRelease, shape, voice.pitch, rate, true);
                enter(layer.modulation, kRelease, layer.zone->modulation, voice.pitch, rate, false);
            }
        }
        state.cut = false;
    }
    const float gain = dsp::dbToGainF(render.param(idx(P::Gain)));
    // The part's mix and mod wheel: live MIDI's if it set them, else the parameters
    // (a single-part channel has only its wheel; its mix is the mixer's).
    const bool parts = render.param(idx(P::Part)) >= 0.5F;
    const Live& live = m_live[state.part];
    const auto partParam = [&](GmPartParam which, float single) {
        return parts ? render.param(gmPartParam(state.part, which)) : single;
    };
    const PartMix mix = partMix(live.volume >= 0 ? static_cast<float>(live.volume)
                                                 : partParam(GmPartParam::Volume, 100.0F),
                                live.expression >= 0 ? static_cast<float>(live.expression)
                                                     : partParam(GmPartParam::Expression, 127.0F),
                                live.panSet ? live.pan : partParam(GmPartParam::Pan, 0.0F));
    const float wheel = live.wheel >= 0
                            ? static_cast<float>(live.wheel)
                            : partParam(GmPartParam::ModWheel, render.param(idx(P::ModWheel)));
    const float wheelCents = kWheelVibratoCents * std::clamp(wheel, 0.0F, 127.0F) / 127.0F;
    const float inverseRate = 1.0F / rate;
    bool alive = false;
    float level = 0.0F;
    for (std::uint32_t l = 0; l < state.layerCount; ++l) {
        GmLayer& layer = state.layers[l];
        if (!layer.active) {
            continue;
        }
        const BankZone& zone = *layer.zone;
        const std::span<const float> data{layer.sample.data, layer.sample.frames};
        const auto end = static_cast<double>(std::min<std::size_t>(zone.end, data.size()));
        const auto loopStart = static_cast<double>(zone.loopStart);
        const auto loopEnd = static_cast<double>(std::min<std::size_t>(zone.loopEnd, data.size()));
        for (std::uint32_t i = 0; i < left.size(); ++i) {
            const float env = next(layer.volume, zone.volume, voice.pitch, rate, true);
            const float mod = next(layer.modulation, zone.modulation, voice.pitch, rate, false);
            float vib = 0.0F;
            if (layer.vibDelay > 0) {
                --layer.vibDelay;
            } else {
                vib = triangle(layer.vibPhase);
                layer.vibPhase += zone.vibrato.hz * inverseRate;
            }
            float lfo = 0.0F;
            if (layer.modDelay > 0) {
                --layer.modDelay;
            } else {
                lfo = triangle(layer.modPhase);
                layer.modPhase += zone.modLfo.hz * inverseRate;
            }
            if (control(voice, i)) {
                const float cents = i < render.pitchCents.size() ? render.pitchCents[i] : 0.0F;
                refreshLayer(layer, layer.baseCents + cents + (mod * zone.modToPitch) +
                                        (vib * (zone.vibrato.toPitch + wheelCents)) +
                                        (lfo * zone.modLfo.toPitch));
                if (layer.filtered && (zone.modToFilter != 0.0F || zone.modLfo.toFilter != 0.0F)) {
                    const float cutoff =
                        zone.filterHz *
                        dsp::exp2F(((mod * zone.modToFilter) + (lfo * zone.modLfo.toFilter)) /
                                   1200.0F);
                    layer.coefficients = dsp::svfCoefficients(
                        std::clamp(static_cast<double>(cutoff), 20.0, 0.45 * sampleRate()),
                        0.70710678 * dsp::dbToGain(static_cast<double>(zone.filterQ)),
                        static_cast<double>(sampleRate()));
                }
            }
            // Looping: continuous always; sustain while the note is held.
            const bool looping = (zone.loop == BankLoop::Continuous ||
                                  (zone.loop == BankLoop::Sustain && !state.released)) &&
                                 loopEnd > loopStart + 1.0;
            if (looping && layer.position >= loopEnd) {
                layer.position -=
                    (loopEnd - loopStart) *
                    dsp::floorExact((layer.position - loopStart) / (loopEnd - loopStart));
            }
            if (!looping && layer.position >= end - 1.0) {
                layer.active = false;
                break;
            }
            const auto at = static_cast<std::size_t>(layer.position);
            const auto fraction = static_cast<float>(layer.position - static_cast<double>(at));
            // Across a loop's seam the next frame is the loop's first.
            const std::size_t after =
                looping && static_cast<double>(at + 1) >= loopEnd ? zone.loopStart : at + 1;
            float s = data[at] + ((data[std::min(after, data.size() - 1)] - data[at]) * fraction);
            if (layer.filtered) {
                s = layer.filter[0].process(s, layer.coefficients, dsp::SvfMode::LowPass);
            }
            float amp = env * gain;
            if (zone.modLfo.toVolume != 0.0F) {
                amp *= dsp::dbToGainF(lfo * zone.modLfo.toVolume);
            }
            left[i] += s * amp * layer.gainLeft * mix.left;
            right[i] += s * amp * layer.gainRight * mix.right;
            layer.position += static_cast<double>(layer.increment);
            if (layer.volume.stage == kDone) {
                layer.active = false;
                break;
            }
        }
        level = std::max(level, layer.volume.level);
        alive = alive || layer.active;
    }
    voice.level = level;
    return alive;
}

} // namespace adx::instruments
