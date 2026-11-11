#include "engine/instruments/gm/GmInstrument.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "engine/dsp/Math.h"
#include "engine/format/soundfont/SoundFont.h"

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

[[nodiscard]] constexpr std::uint32_t idx(P p) noexcept {
    return static_cast<std::uint32_t>(p);
}

[[nodiscard]] int choice(const graph::VoiceRender& render, P p, int highest) noexcept {
    return std::clamp(static_cast<int>(dsp::floorExact(render.param(idx(p)) + 0.5F)), 0, highest);
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

void enter(GmEnvelope& env, std::uint8_t stage, const BankEnvelope& shape, std::uint8_t key,
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
        env.step = volume ? fallFactor(seconds, rate)
                          : (env.left > 0 ? 1.0F / static_cast<float>(env.left) : 1.0F);
        break;
    }
    case kRelease:
        env.left = frames(shape.release, rate);
        env.step = volume ? fallFactor(shape.release, rate)
                          : (env.left > 0 ? env.level / static_cast<float>(env.left) : 1.0F);
        break;
    default:
        break;
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
    return p < 0.25F ? 4.0F * p : (p < 0.75F ? 2.0F - (4.0F * p) : (4.0F * p) - 4.0F);
}

struct CacheEntry {
    std::weak_ptr<const format::soundfont::Bank> bank;
};

std::mutex& cacheMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, CacheEntry>& cache() {
    static std::map<std::string, CacheEntry> entries;
    return entries;
}

} // namespace

std::shared_ptr<const format::soundfont::Bank> GmBankCache::get(const std::filesystem::path& path,
                                                                std::string& error) {
    const std::string key = path.lexically_normal().generic_string();
    {
        const std::lock_guard lock(cacheMutex());
        if (auto held = cache()[key].bank.lock()) {
            return held;
        }
    }
    auto bank = std::make_shared<format::soundfont::Bank>();
    if (!format::soundfont::loadBank(path, *bank, error)) {
        return nullptr;
    }
    std::shared_ptr<const format::soundfont::Bank> shared = std::move(bank);
    const std::lock_guard lock(cacheMutex());
    if (auto raced = cache()[key].bank.lock()) {
        return raced;
    }
    cache()[key].bank = shared;
    return shared;
}

std::filesystem::path gmBankPath(const std::string& written, const std::filesystem::path& base) {
    if (written.empty() || written == "system:gm.dls") {
        return format::soundfont::systemGmBank();
    }
    std::filesystem::path path(written);
    if (path.is_relative() && !base.empty()) {
        path = base / path;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) ? path : std::filesystem::path{};
}

void configureGm(GmInstrument& node, const project::Channel& channel,
                 const std::filesystem::path& base) {
    const std::filesystem::path path = gmBankPath(channel.instrument.voicebank, base);
    std::string error;
    node.setBank(path.empty() ? nullptr : GmBankCache::get(path, error), path);
}

bool gmMatches(const GmInstrument& node, const project::Channel& channel,
               const std::filesystem::path& base) {
    return node.bankPath() == gmBankPath(channel.instrument.voicebank, base);
}

void GmInstrument::setBank(std::shared_ptr<const format::soundfont::Bank> bank,
                           std::filesystem::path path) noexcept {
    m_bank = std::move(bank);
    m_path = std::move(path);
}

void GmInstrument::refreshLayer(GmLayer& layer, float cents) const noexcept {
    layer.increment =
        static_cast<float>(layer.rateRatio * static_cast<double>(dsp::exp2F(cents / 1200.0F)));
}

void GmInstrument::startLayer(GmLayer& layer, const BankZone& zone, const graph::Voice& voice,
                              float velocity, float tune) const noexcept {
    const auto rate = static_cast<float>(sampleRate());
    layer.zone = &zone;
    layer.sample = &m_bank->samples[zone.sample];
    layer.position = static_cast<double>(zone.start);
    layer.rateRatio = static_cast<double>(layer.sample->rate) / static_cast<double>(sampleRate());
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
    layer.gainLeft = gain * dsp::cosTurnsF((pan + 1.0F) * 0.125F) * 1.41421356F;
    layer.gainRight = gain * dsp::sinTurnsF((pan + 1.0F) * 0.125F) * 1.41421356F;
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
    layer.active = !layer.sample->data.empty() && zone.end > zone.start;
}

void GmInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                              const graph::VoiceRender& render) noexcept {
    GmVoice& state = stateOf(voice);
    state.layerCount = 0;
    state.released = false;
    state.cut = false;
    state.exclusiveClass = 0;
    state.velocity = std::clamp(render.param(idx(P::Velocity)), 0.0F, 1.0F);
    voice.level = 0.0F;
    if (m_bank == nullptr) {
        return;
    }
    const bool drums = render.param(idx(P::Drums)) >= 0.5F;
    const auto bank = static_cast<std::uint16_t>(drums ? format::soundfont::Bank::kDrumBank
                                                       : (choice(render, P::BankMsb, 127) * 128) +
                                                             choice(render, P::BankLsb, 127));
    const auto program = static_cast<std::uint8_t>(choice(render, P::Program, 127));
    const format::soundfont::BankPreset* preset = m_bank->find(bank, program);
    if (preset == nullptr) {
        return;
    }
    const float tune = render.param(idx(P::Tune));
    for (const BankZone& zone : preset->zones) {
        if (state.layerCount >= kGmLayers) {
            break;
        }
        if (!zone.matches(voice.pitch, voice.velocity) || zone.sample >= m_bank->samples.size()) {
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
                if (them.exclusiveClass == state.exclusiveClass) {
                    them.cut = true;
                }
            }
        }
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
    const float inverseRate = 1.0F / rate;
    bool alive = false;
    float level = 0.0F;
    for (std::uint32_t l = 0; l < state.layerCount; ++l) {
        GmLayer& layer = state.layers[l];
        if (!layer.active) {
            continue;
        }
        const BankZone& zone = *layer.zone;
        const std::vector<float>& data = layer.sample->data;
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
                                        (vib * zone.vibrato.toPitch) + (lfo * zone.modLfo.toPitch));
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
            left[i] += s * amp * layer.gainLeft;
            right[i] += s * amp * layer.gainRight;
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
