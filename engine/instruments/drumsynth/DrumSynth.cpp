#include "engine/instruments/drumsynth/DrumSynth.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"
#include "engine/dsp/Saturate.h"

namespace adx::instruments {
namespace {

using P = DrumParam;

[[nodiscard]] constexpr std::uint32_t idx(P param) noexcept {
    return static_cast<std::uint32_t>(param);
}

/// The TR-808's six hat oscillators, Hz.
constexpr std::array<float, 6> kMetal{205.3F, 304.4F, 369.6F, 522.7F, 540.0F, 800.0F};
/// The clap's bursts, seconds from the hit.
constexpr std::array<float, 3> kClapBursts{0.0F, 0.011F, 0.023F};
/// Below this a voice is finished: 80 dB down.
constexpr float kSilence = 1e-4F;

/// Per-sample multiplier that falls 60 dB in `seconds`.
/// How fast a choked open hat falls silent.
constexpr float kHatChokeSeconds = 0.004F;

[[nodiscard]] float decayPerSample(float seconds, std::uint32_t rate) noexcept {
    const double samples = std::max(1.0, static_cast<double>(seconds) * rate);
    return static_cast<float>(dsp::exp(-6.907755278982137 / samples));
}

/// Per-sample multiplier with time constant `seconds`.
[[nodiscard]] float timeConstant(float seconds, std::uint32_t rate) noexcept {
    const double samples = std::max(1.0, static_cast<double>(seconds) * rate);
    return static_cast<float>(dsp::exp(-1.0 / samples));
}

[[nodiscard]] std::uint32_t voiceSeed(const graph::Voice& voice) noexcept {
    std::uint32_t h = 2166136261U;
    for (const std::uint32_t part : {voice.key.channelId, voice.key.noteId, voice.key.instance}) {
        h = (h ^ part) * 16777619U;
    }
    return h == 0 ? 1U : h;
}

/// Resting pitch of each model at key 60, Hz.
[[nodiscard]] float baseFrequency(DrumModel model) noexcept {
    switch (model) {
    case DrumModel::Kick:
        return 50.0F;
    case DrumModel::Snare:
        return 180.0F;
    case DrumModel::Tom:
        return 110.0F;
    case DrumModel::Rim:
        return 1700.0F;
    case DrumModel::Clave:
        return 2500.0F;
    case DrumModel::Conga:
        return 200.0F;
    default:
        return 1.0F; // hat and clap scale their partials or filters by the ratio
    }
}

} // namespace

DrumModel kitModel(std::uint8_t key) noexcept {
    switch (key) {
    case 35:
    case 36:
        return DrumModel::Kick;
    case 37:
    case 38:
    case 40:
        return DrumModel::Snare;
    case 39:
        return DrumModel::Clap;
    case 42:
    case 44:
    case 46:
        return DrumModel::Hat;
    case 41:
    case 43:
    case 45:
    case 47:
    case 48:
    case 50:
        return DrumModel::Tom;
    // The 808's percussion, on General MIDI's keys (phase_12.md §4.5).
    case 56:
        return DrumModel::Cowbell;
    case 49:
    case 51:
    case 52:
    case 53:
    case 55:
    case 57:
    case 59:
        return DrumModel::Cymbal;
    case 75:
    case 76:
    case 77:
        return DrumModel::Clave;
    case 69:
    case 70:
    case 82:
        return DrumModel::Shaker;
    case 60:
    case 61:
    case 62:
    case 63:
    case 64:
        return DrumModel::Conga;
    default:
        // Elsewhere on the keyboard the five models repeat, so every key sounds.
        return static_cast<DrumModel>(key % 5);
    }
}

void DrumSynth::prepareInstrument(const graph::PrepareInfo& /*info*/) {
    dsp::Oscillator::prepareTables();
}

// NOLINTNEXTLINE(readability-function-size) - five models' set-up in one place.
void DrumSynth::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                           const graph::VoiceRender& render) noexcept {
    DrumVoice& s = stateOf(voice);
    const std::uint32_t rate = sampleRate();
    const int modelParam =
        std::clamp(static_cast<int>(dsp::floorExact(render.param(idx(P::Model)) + 0.5F)), 0,
                   kDrumModelCount - 1);
    const bool kit = modelParam == static_cast<int>(DrumModel::Kit);
    s.model = kit ? kitModel(voice.pitch) : static_cast<DrumModel>(modelParam);
    // In a kit only toms are tuned by their key, as a kit's toms are.
    float keyTrack = render.param(idx(P::KeyTrack));
    if (kit) {
        // Toms and congas are tuned by their key, as a kit's are.
        keyTrack = (s.model == DrumModel::Tom || s.model == DrumModel::Conga) ? 1.0F : 0.0F;
    }
    const float semitones =
        render.param(idx(P::Tune)) + (keyTrack * (static_cast<float>(voice.pitch) - 60.0F));
    const float ratio = dsp::exp2F(semitones / 12.0F);
    s.frequency = baseFrequency(s.model) * ratio;

    float decay = std::max(0.01F, render.param(idx(P::Decay)));
    // A hat voice is a pair: its open key rings for its own time, and a closed or pedal
    // hit cuts an open one still ringing (phase_13.md §4.2). Both off by default.
    if (s.model == DrumModel::Hat) {
        const float open = render.param(idx(P::HatOpenDecay));
        if (open > 0.0F && voice.pitch == kOpenHatKey) {
            decay = std::max(0.01F, open);
        }
        if (render.param(idx(P::HatChoke)) >= 0.5F &&
            (voice.pitch == kClosedHatKey || voice.pitch == kPedalHatKey)) {
            for (graph::Voice& other : pool().voices()) {
                if (&other != &voice && other.phase != graph::VoicePhase::Free &&
                    other.pitch == kOpenHatKey && stateOf(other).model == DrumModel::Hat) {
                    stateOf(other).ampDecay = decayPerSample(kHatChokeSeconds, rate);
                }
            }
        }
    }
    const float tone = std::clamp(render.param(idx(P::Tone)), 0.0F, 1.0F);
    const float click = std::clamp(render.param(idx(P::Click)), 0.0F, 1.0F);
    const float noise = std::clamp(render.param(idx(P::Noise)), 0.0F, 1.0F);
    s.pitchDrop = std::max(0.0F, render.param(idx(P::PitchDrop)));
    const float pitchTime = std::max(0.001F, render.param(idx(P::PitchTime)));
    s.subLevel = std::clamp(render.param(idx(P::Sub)), 0.0F, 1.0F);
    const float drive = std::max(0.0F, render.param(idx(P::Drive)));
    s.drive = 1.0F + drive;
    s.driveNorm = drive > 0.0F ? dsp::driveNorm(s.drive) : 1.0F;
    s.velocity = static_cast<float>(voice.velocity) / 127.0F;
    s.character =
        std::clamp(static_cast<int>(dsp::floorExact(render.param(idx(P::Character)) + 0.5F)), 0, 2);
    s.fast = 1.0F;
    s.fastDecay = 0.0F;

    s.amp = 1.0F;
    s.ampDecay = decayPerSample(decay, rate);
    s.toneAmp = 1.0F;
    s.toneDecay = s.ampDecay;
    s.sweep = 1.0F;
    s.sweepDecay = timeConstant(pitchTime, rate);
    s.click = 1.0F;
    s.clickDecay = timeConstant(0.002F, rate);
    s.clickLevel = click;
    s.noiseLevel = noise;
    s.phase = 0.0F;
    s.phase2 = 0.0F;
    s.subPhase = 0.0F;
    s.age = 0;
    s.noise.reseed(voiceSeed(voice));
    s.filter.reset();
    s.clickFilter.reset();
    s.highPass.reset();
    const auto rateD = static_cast<double>(rate);
    s.clickCoefficients =
        dsp::svfCoefficients(std::min(2000.0 + (8000.0 * tone), rateD * 0.45), 0.7, rateD);

    switch (s.model) {
    case DrumModel::Kick:
        if (s.character == 1) { // 909: a faster drop and a harder click
            s.sweepDecay = timeConstant(pitchTime * 0.6F, rate);
            s.clickLevel = std::min(1.0F, click * 1.5F);
        } else if (s.character == 2) { // 707: shorter
            s.ampDecay = decayPerSample(decay * 0.7F, rate);
            s.toneDecay = s.ampDecay;
        }
        break;
    case DrumModel::Cowbell:
    case DrumModel::Cymbal:
        for (std::size_t m = 0; m < s.metal.size(); ++m) {
            const float phase = static_cast<float>(m) * 0.381966F;
            s.metal[m].reset(phase - dsp::floorExact(phase));
        }
        if (s.model == DrumModel::Cowbell) {
            s.fastDecay = timeConstant(0.015F, rate);
            s.filterCoefficients = dsp::svfCoefficients(
                std::min((2000.0 + (1200.0 * tone)) * ratio, rateD * 0.45), 1.4, rateD);
        } else {
            s.fastDecay = decayPerSample(std::max(0.05F, decay * 0.25F), rate);
            s.filterCoefficients =
                dsp::svfCoefficients(std::min(3400.0 * ratio, rateD * 0.45), 1.0, rateD);
            s.highPassCoefficients =
                dsp::svfCoefficients(std::min(7000.0 * ratio, rateD * 0.45), 0.7, rateD);
        }
        break;
    case DrumModel::Rim:
    case DrumModel::Clave: {
        const float longest = s.model == DrumModel::Rim ? 0.04F : 0.06F;
        const float shortest = s.model == DrumModel::Rim ? 0.01F : 0.02F;
        s.ampDecay = decayPerSample(std::clamp(decay * 0.1F, shortest, longest), rate);
        s.toneDecay = s.ampDecay;
        s.clickDecay = timeConstant(0.001F, rate);
        s.clickLevel = s.model == DrumModel::Rim ? std::max(click, 0.3F) : click * 0.3F;
        break;
    }
    case DrumModel::Shaker:
        s.fastDecay = timeConstant(0.005F, rate); // the rise: 1 - fast
        s.filterCoefficients =
            dsp::svfCoefficients(std::min(3000.0 + (9000.0 * tone), rateD * 0.45), 0.7, rateD);
        break;
    case DrumModel::Conga:
        s.pitchDrop = std::min(s.pitchDrop, 6.0F);
        s.sweepDecay = timeConstant(std::min(pitchTime, 0.01F), rate);
        s.filterCoefficients =
            dsp::svfCoefficients(std::min(s.frequency * 3.0, rateD * 0.45), 2.5, rateD);
        break;
    case DrumModel::Snare:
        // The tones die in a third of the noise's time, as a real snare's shell does.
        s.toneDecay = decayPerSample(decay * 0.35F, rate);
        s.pitchDrop = std::min(s.pitchDrop, 4.0F);
        s.filterCoefficients =
            dsp::svfCoefficients(std::min(1000.0 + (7000.0 * tone), rateD * 0.45), 0.9, rateD);
        break;
    case DrumModel::Hat:
        for (std::size_t m = 0; m < s.metal.size(); ++m) {
            const float phase = static_cast<float>(m) * 0.381966F;
            s.metal[m].reset(phase - dsp::floorExact(phase));
        }
        s.filterCoefficients = dsp::svfCoefficients(
            std::min((7000.0 + (5000.0 * tone)) * ratio, rateD * 0.45), 1.2, rateD);
        s.highPassCoefficients =
            dsp::svfCoefficients(std::min(6000.0 * ratio, rateD * 0.45), 0.7, rateD);
        break;
    case DrumModel::Clap:
        s.filterCoefficients = dsp::svfCoefficients(
            std::min((800.0 + (2400.0 * tone)) * ratio, rateD * 0.45), 1.6, rateD);
        s.toneDecay = timeConstant(0.005F, rate); // each burst's own fall
        break;
    case DrumModel::Tom:
        s.pitchDrop *= 0.25F;
        s.filterCoefficients =
            dsp::svfCoefficients(std::min(s.frequency * 4.0, rateD * 0.45), 0.8, rateD);
        break;
    case DrumModel::Kit:
        break;
    }
    voice.level = s.velocity;
}

// NOLINTNEXTLINE(readability-function-size) - one voice, five models.
bool DrumSynth::renderVoice(graph::Voice& voice, std::span<float> left, std::span<float> right,
                            const graph::VoiceRender& render) noexcept {
    DrumVoice& s = stateOf(voice);
    const float inverseRate = 1.0F / static_cast<float>(sampleRate());
    const bool levelAutomated =
        idx(P::Level) < render.automation.size() && render.automation[idx(P::Level)] != nullptr;
    const float level = dsp::dbToGainF(render.param(idx(P::Level)));
    const auto rate = static_cast<float>(sampleRate());

    bool alive = true;
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        const float cents = i < render.pitchCents.size() ? render.pitchCents[i] : 0.0F;
        const float bend = cents == 0.0F ? 1.0F : dsp::exp2F(cents / 1200.0F);
        float out = 0.0F;
        const float clickNoise =
            s.clickFilter.process(s.noise.next(), s.clickCoefficients, dsp::SvfMode::HighPass);
        const float transient = clickNoise * s.click * s.clickLevel;

        switch (s.model) {
        case DrumModel::Kick:
        case DrumModel::Tom: {
            const float hz = s.frequency * bend * dsp::exp2F(s.pitchDrop * s.sweep / 12.0F);
            s.phase += hz * inverseRate;
            s.phase -= dsp::floorExact(s.phase);
            float body = dsp::sinTurnsF(s.phase);
            if (s.subLevel > 0.0F) {
                s.subPhase += s.frequency * bend * inverseRate;
                s.subPhase -= dsp::floorExact(s.subPhase);
                body += dsp::sinTurnsF(s.subPhase) * s.subLevel;
            }
            out = (body + transient) * s.amp;
            if (s.model == DrumModel::Tom) {
                const float skin =
                    s.filter.process(s.noise.next(), s.filterCoefficients, dsp::SvfMode::BandPass);
                out += skin * s.noiseLevel * 0.3F * s.amp;
            }
            break;
        }
        case DrumModel::Snare: {
            const float dip = dsp::exp2F(s.pitchDrop * s.sweep / 12.0F) * bend;
            s.phase += s.frequency * dip * inverseRate;
            s.phase -= dsp::floorExact(s.phase);
            s.phase2 += s.frequency * (330.0F / 180.0F) * dip * inverseRate;
            s.phase2 -= dsp::floorExact(s.phase2);
            const float shell = (dsp::sinTurnsF(s.phase) + (0.6F * dsp::sinTurnsF(s.phase2))) *
                                0.6F * s.toneAmp * (1.0F - (0.5F * s.noiseLevel));
            const float wires =
                s.filter.process(s.noise.next(), s.filterCoefficients, dsp::SvfMode::BandPass);
            out = shell + (((wires * 2.0F * s.noiseLevel) + transient) * s.amp);
            break;
        }
        case DrumModel::Hat: {
            float metal = 0.0F;
            for (std::size_t m = 0; m < s.metal.size(); ++m) {
                const float hz = std::min(kMetal[m] * (s.frequency) * bend, rate * 0.45F);
                metal += s.metal[m].nextBandLimited(dsp::OscShape::Square, hz * inverseRate);
            }
            metal = s.filter.process(metal * 0.3F, s.filterCoefficients, dsp::SvfMode::BandPass);
            metal = s.highPass.process(metal, s.highPassCoefficients, dsp::SvfMode::HighPass);
            const float sizzle = s.noise.next() * 0.15F * s.noiseLevel;
            out = (metal * 2.0F + sizzle + (transient * 0.5F)) * s.amp;
            break;
        }
        case DrumModel::Clap: {
            const float t = static_cast<float>(s.age) * inverseRate;
            // Restart the burst envelope at each burst; after the last, the tail.
            for (const float burst : kClapBursts) {
                if (s.age == static_cast<std::uint32_t>(burst * rate)) {
                    s.toneAmp = 1.0F;
                }
            }
            const float raw =
                s.filter.process(s.noise.next(), s.filterCoefficients, dsp::SvfMode::BandPass);
            const float tail = t >= kClapBursts.back() ? 0.5F * s.amp : 0.0F;
            out = raw * 2.5F * (s.toneAmp + tail);
            break;
        }
        case DrumModel::Cowbell: {
            float bell = 0.0F;
            for (std::size_t m = 4; m < 6; ++m) {
                const float hz = std::min(kMetal[m] * s.frequency * bend, rate * 0.45F);
                bell += s.metal[m].nextBandLimited(dsp::OscShape::Square, hz * inverseRate);
            }
            bell = s.filter.process(bell * 0.5F, s.filterCoefficients, dsp::SvfMode::BandPass);
            out = bell * ((0.6F * s.fast) + (0.4F * s.amp)) * 2.0F;
            break;
        }
        case DrumModel::Cymbal: {
            float metal = 0.0F;
            for (std::size_t m = 0; m < s.metal.size(); ++m) {
                const float hz = std::min(kMetal[m] * s.frequency * bend, rate * 0.45F);
                metal += s.metal[m].nextBandLimited(dsp::OscShape::Square, hz * inverseRate);
            }
            metal *= 0.25F;
            const float body =
                s.filter.process(metal, s.filterCoefficients, dsp::SvfMode::BandPass) * s.amp;
            const float high = s.highPass.process(metal + (s.noise.next() * 0.2F * s.noiseLevel),
                                                  s.highPassCoefficients, dsp::SvfMode::HighPass) *
                               s.fast;
            out = (body + high) * 1.5F;
            break;
        }
        case DrumModel::Rim:
        case DrumModel::Clave: {
            s.phase += s.frequency * bend * inverseRate;
            s.phase -= dsp::floorExact(s.phase);
            out = (dsp::sinTurnsF(s.phase) * s.amp) + (transient * 2.0F);
            break;
        }
        case DrumModel::Shaker: {
            const float grains =
                s.filter.process(s.noise.next(), s.filterCoefficients, dsp::SvfMode::HighPass);
            out = grains * (1.0F - s.fast) * s.amp * 1.5F;
            break;
        }
        case DrumModel::Conga: {
            const float hz = s.frequency * bend * dsp::exp2F(s.pitchDrop * s.sweep / 12.0F);
            s.phase += hz * inverseRate;
            s.phase -= dsp::floorExact(s.phase);
            const float skin =
                s.filter.process(s.noise.next(), s.filterCoefficients, dsp::SvfMode::BandPass);
            out = ((dsp::sinTurnsF(s.phase) + (skin * s.noiseLevel * 0.2F)) * s.amp) +
                  (transient * 0.3F);
            break;
        }
        case DrumModel::Kit:
            break;
        }

        if (s.drive > 1.0F) {
            out = dsp::driveTanh(out, s.drive, s.driveNorm);
        }
        const float gain =
            s.velocity *
            (levelAutomated ? dsp::dbToGainF(render.paramAt(idx(P::Level), i)) : level);
        left[i] = out * gain;
        right[i] = out * gain;

        s.amp *= s.ampDecay;
        s.toneAmp *= s.toneDecay;
        s.sweep *= s.sweepDecay;
        s.click *= s.clickDecay;
        s.fast *= s.fastDecay;
        ++s.age;
        // The clap's bursts must all have fired before its level can count as gone.
        const bool bursting = s.model == DrumModel::Clap &&
                              static_cast<float>(s.age) * inverseRate <= kClapBursts.back();
        if (s.amp < kSilence && s.toneAmp < kSilence && !bursting) {
            alive = false;
        }
    }
    voice.level = s.amp * s.velocity;
    return alive;
}

} // namespace adx::instruments
