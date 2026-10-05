#include "engine/instruments/granular/GranularInstrument.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"
#include "engine/dsp/Oscillator.h"
#include "engine/dsp/WaveTable.h"

namespace adx::instruments {
namespace {

using P = GranularParam;

[[nodiscard]] constexpr std::uint32_t idx(P param) noexcept {
    return static_cast<std::uint32_t>(param);
}

[[nodiscard]] int stepped(const graph::VoiceRender& render, P param, std::uint32_t frame) noexcept {
    return static_cast<int>(std::floor(render.paramAt(idx(param), frame) + 0.5F));
}

[[nodiscard]] std::uint32_t voiceSeed(const graph::Voice& voice) noexcept {
    std::uint32_t h = 2166136261U;
    for (const std::uint32_t part : {voice.key.channelId, voice.key.noteId, voice.key.instance}) {
        h = (h ^ part) * 16777619U;
    }
    return h == 0 ? 1U : h;
}

/// A linear read with the buffer's guard frames covering index + 1.
[[nodiscard]] float readLinear(const float* data, double position) noexcept {
    const auto index = static_cast<std::size_t>(position);
    const auto fraction = static_cast<float>(position - static_cast<double>(index));
    return data[index] + ((data[index + 1] - data[index]) * fraction);
}

} // namespace

float grainWindow(GrainWindow window, float t) noexcept {
    t = std::clamp(t, 0.0F, 1.0F);
    switch (window) {
    case GrainWindow::Tukey: {
        // Flat top, a raised-cosine quarter at each end.
        constexpr float kTaper = 0.25F;
        if (t < kTaper) {
            return 0.5F - (0.5F * dsp::cosTurnsF(t / kTaper * 0.5F));
        }
        if (t > 1.0F - kTaper) {
            return 0.5F - (0.5F * dsp::cosTurnsF((1.0F - t) / kTaper * 0.5F));
        }
        return 1.0F;
    }
    case GrainWindow::Triangle:
        return 1.0F - std::abs((2.0F * t) - 1.0F);
    case GrainWindow::Hann:
        break;
    }
    return 0.5F - (0.5F * dsp::cosTurnsF(t));
}

void GranularInstrument::prepareInstrument(const graph::PrepareInfo& /*info*/) {
    dsp::Oscillator::prepareTables();
}

void GranularInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                                    const graph::VoiceRender& render) noexcept {
    GranularVoice& state = stateOf(voice);
    for (Grain& grain : state.grains) {
        grain.active = false;
    }
    state.zone = nullptr;
    for (const SamplerZone& zone : zones()) {
        if (zone.zone.matches(voice.pitch, voice.velocity)) {
            state.zone = &zone;
            break;
        }
    }
    state.random.reseed(voiceSeed(voice));
    state.untilNext = 0.0; // the first grain starts with the note
    state.frames = 0;
    state.velocity = static_cast<float>(voice.velocity) / 127.0F;
    state.released = false;
    state.shape.attackSeconds = render.param(idx(P::EnvAttack));
    state.shape.decaySeconds = render.param(idx(P::EnvDecay));
    state.shape.sustain = render.param(idx(P::EnvSustain));
    state.shape.releaseSeconds = render.param(idx(P::EnvRelease));
    state.env.reset();
    state.env.trigger(state.shape, sampleRate());
    voice.level = 0.0F;
}

void GranularInstrument::spawn(graph::Voice& voice, GranularVoice& state,
                               const graph::VoiceRender& render, std::uint32_t frame) noexcept {
    Grain* slot = nullptr;
    for (Grain& grain : state.grains) {
        if (!grain.active) {
            slot = &grain;
            break;
        }
    }
    // The randomness is drawn whether or not a slot is free, so a full pool never
    // shifts the sequence the later grains see.
    const float sprayDraw = state.random.nextUnit();
    const float jitterDraw = state.random.next();
    const float panDraw = state.random.next();
    if (slot == nullptr) {
        return;
    }
    const auto rate = static_cast<double>(sampleRate());
    const float size = std::clamp(render.paramAt(idx(P::Size), frame), 5.0F, 500.0F);
    const float jitter = render.paramAt(idx(P::PitchJitter), frame) * jitterDraw;
    const float cents =
        (frame < render.pitchCents.size() ? render.pitchCents[frame] : 0.0F) + jitter;
    const float spread = std::clamp(render.paramAt(idx(P::Spread), frame), 0.0F, 1.0F);
    const float pan = panDraw * spread; // -1 .. 1
    slot->gainLeft = dsp::cosTurnsF((pan + 1.0F) * 0.125F);
    slot->gainRight = dsp::sinTurnsF((pan + 1.0F) * 0.125F);
    slot->length = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(size * 0.001 * rate));
    slot->age = 0;

    const auto source = static_cast<GrainSource>(
        std::clamp(stepped(render, P::Source, frame), 0, kGrainSourceCount - 1));
    const bool sample = source == GrainSource::Sample && state.zone != nullptr &&
                        state.zone->sample != nullptr && state.zone->sample->ready();
    if (sample) {
        const format::SampleView view = state.zone->sample->view();
        const project::SampleZone& map = state.zone->zone;
        const float semitones = static_cast<float>(voice.pitch) - static_cast<float>(map.rootKey) +
                                ((cents + map.tuneCents) * 0.01F);
        slot->increment = (static_cast<double>(view.sampleRate) / rate) *
                          static_cast<double>(dsp::exp2F(semitones / 12.0F));
        const auto frames = static_cast<double>(view.frames);
        const double scanned = static_cast<double>(render.paramAt(idx(P::Position), frame)) +
                               (static_cast<double>(render.paramAt(idx(P::Scan), frame)) *
                                static_cast<double>(state.frames) / rate);
        const double start = ((scanned - std::floor(scanned)) * frames) +
                             (static_cast<double>(render.paramAt(idx(P::Spray), frame)) *
                              sprayDraw * static_cast<double>(view.sampleRate));
        // Keep the whole grain inside the sample.
        const double span = slot->increment * slot->length;
        slot->position = std::clamp(start, 0.0, std::max(0.0, frames - span - 1.0));
        slot->active = span < frames - 1.0;
    } else {
        const float hz = dsp::midiToHz(static_cast<float>(voice.pitch) + (cents * 0.01F));
        slot->increment = static_cast<double>(hz) / rate;
        slot->position = sprayDraw; // a random phase, so overlapping grains do not comb
        slot->active = true;
    }
}

// NOLINTNEXTLINE(readability-function-size) - one voice: scheduler, grains, envelope.
bool GranularInstrument::renderVoice(graph::Voice& voice, std::span<float> left,
                                     std::span<float> right,
                                     const graph::VoiceRender& render) noexcept {
    GranularVoice& state = stateOf(voice);
    if (voice.phase == graph::VoicePhase::Released && !state.released) {
        state.released = true;
        state.env.release(state.shape, sampleRate());
    }
    const auto rate = static_cast<double>(sampleRate());
    const auto source = static_cast<GrainSource>(
        std::clamp(stepped(render, P::Source, 0), 0, kGrainSourceCount - 1));
    const auto window = static_cast<GrainWindow>(
        std::clamp(stepped(render, P::Window, 0), 0, kGrainWindowCount - 1));
    const bool sample = source == GrainSource::Sample && state.zone != nullptr &&
                        state.zone->sample != nullptr && state.zone->sample->ready();
    const format::SampleView view = sample ? state.zone->sample->view() : format::SampleView{};
    const dsp::WaveTable& saw = dsp::builtinWaveTable(dsp::Waveform::Saw);
    const float level = dsp::dbToGainF(render.param(idx(P::Level)));
    const bool levelAutomated =
        idx(P::Level) < render.automation.size() && render.automation[idx(P::Level)] != nullptr;

    bool alive = true;
    float env = state.env.level();
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        const double density =
            std::clamp(static_cast<double>(render.paramAt(idx(P::Density), i)), 1.0, 200.0);
        if (state.untilNext <= 0.0) {
            spawn(voice, state, render, i);
            state.untilNext += rate / density;
        }
        state.untilNext -= 1.0;

        float l = 0.0F;
        float r = 0.0F;
        for (Grain& grain : state.grains) {
            if (!grain.active) {
                continue;
            }
            const float w = grainWindow(window, static_cast<float>(grain.age) /
                                                    static_cast<float>(grain.length - 1));
            float s = 0.0F;
            if (sample) {
                s = readLinear(view.left, grain.position);
                const float sr =
                    view.right == view.left ? s : readLinear(view.right, grain.position);
                l += s * w * grain.gainLeft;
                r += sr * w * grain.gainRight;
            } else {
                const auto phase = static_cast<float>(grain.position);
                s = source == GrainSource::Saw
                        ? saw.read(0, dsp::waveTableLevel(static_cast<float>(grain.increment)),
                                   phase)
                        : dsp::sinTurnsF(phase);
                l += s * w * grain.gainLeft;
                r += s * w * grain.gainRight;
            }
            grain.position += grain.increment;
            if (!sample && grain.position >= 1.0) {
                grain.position -= 1.0;
            }
            if (++grain.age >= grain.length) {
                grain.active = false;
            }
        }
        // Overlapping grains are uncorrelated: they add in power. Normalise by the
        // square root of the expected overlap so density does not move the level.
        const double overlap =
            density * static_cast<double>(render.paramAt(idx(P::Size), i)) * 0.001;
        const auto norm = static_cast<float>(1.0 / std::sqrt(std::max(1.0, overlap)));

        env = state.env.next(state.shape);
        const float gain =
            env * state.velocity * norm *
            (levelAutomated ? dsp::dbToGainF(render.paramAt(idx(P::Level), i)) : level);
        left[i] = l * gain;
        right[i] = r * gain;
        ++state.frames;
        if (state.env.finished()) {
            alive = false;
        }
    }
    voice.level = env * state.velocity;
    return alive;
}

} // namespace adx::instruments
