#include "engine/instruments/sampler/SamplerInstrument.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "engine/dsp/Math.h"
#include "engine/instruments/sampler/SamplerParams.h"

namespace adx::instruments {
namespace {

using P = SamplerParam;
using project::LoopMode;

[[nodiscard]] constexpr std::uint32_t idx(P param) noexcept {
    return static_cast<std::uint32_t>(param);
}

/// One channel's sample at `position`: copied when the position is a whole frame and
/// the voice is not interpolating, else interpolated. The buffer's guard frames make
/// every tap in range for positions in [0, frames).
[[nodiscard]] float read(const float* data, double position, bool sinc,
                         const dsp::SincTable& table) noexcept {
    const auto index = static_cast<std::size_t>(position);
    const auto fraction = static_cast<float>(position - static_cast<double>(index));
    if (fraction == 0.0F) {
        return data[index];
    }
    return sinc ? dsp::interpolateSinc(table, data, index, fraction)
                : dsp::interpolateLinear(data, index, fraction);
}

/// A zone's place in a dynamic crossfade at `source` (0..127): equal-power in across
/// xfadeIn, out across xfadeOut.
[[nodiscard]] float crossfadeGain(const project::SampleZone& zone, float source) noexcept {
    float gain = 1.0F;
    if (zone.xfadeInLow != 0 || zone.xfadeInHigh != 0) {
        const auto low = static_cast<float>(zone.xfadeInLow);
        const auto high = static_cast<float>(zone.xfadeInHigh);
        if (source <= low) {
            return 0.0F;
        }
        if (source < high) {
            gain *= dsp::sinTurnsF(0.25F * (source - low) / (high - low));
        }
    }
    if (zone.xfadeOutLow != 127 || zone.xfadeOutHigh != 127) {
        const auto low = static_cast<float>(zone.xfadeOutLow);
        const auto high = static_cast<float>(zone.xfadeOutHigh);
        if (source >= high) {
            return 0.0F;
        }
        if (source > low) {
            gain *= dsp::cosTurnsF(0.25F * (source - low) / (high - low));
        }
    }
    return gain;
}

/// One layer's constants for a renderVoice call, and its pitch cache.
struct LayerCall {
    format::SampleView view;
    double frames{0.0};
    double endFade{0.0};
    double loopStart{0.0};
    double loopEnd{0.0};
    double loopLength{0.0};
    double rateRatio{1.0};
    double increment{1.0};
    float rootOffset{0.0F};
    float cents{0.0F};
    bool ready{false};
    bool looping{false};
};

} // namespace

void SamplerInstrument::prepareInstrument(const graph::PrepareInfo& /*info*/) {
    m_sinc = &dsp::sincTable();
}

void SamplerInstrument::reset() noexcept {
    Instrument::reset();
    // The round-robin counters are part of the render: a reset of playback restarts
    // them, so a render from the top chooses the same zones every time.
    m_roundRobin.fill(0);
}

bool SamplerInstrument::switched(const project::SampleZone& zone) const noexcept {
    if (!zone.hasSwitch()) {
        return true;
    }
    // Before any switch is pressed, or after one of another group, the group's lowest
    // key is the articulation: a preset always plays something.
    const std::uint8_t current =
        (m_switch >= zone.switchLow && m_switch <= zone.switchHigh) ? m_switch : zone.switchLow;
    return zone.switchKey == current;
}

bool SamplerInstrument::plays(const project::SampleZone& zone, std::uint8_t key,
                              std::uint8_t velocity) const noexcept {
    return zone.trigger == project::ZoneTrigger::Attack && zone.matches(key, velocity) &&
           switched(zone);
}

const SamplerZone* SamplerInstrument::choose(std::uint8_t key, std::uint8_t velocity) noexcept {
    const std::span<const SamplerZone> zones = this->zones();
    const SamplerZone* first = nullptr;
    for (const SamplerZone& candidate : zones) {
        if (plays(candidate.zone, key, velocity)) {
            first = &candidate;
            break;
        }
    }
    if (first == nullptr || first->zone.roundRobinGroup == 0) {
        return first;
    }
    // The matching members of this group, in index order: the counter picks the next.
    const std::uint8_t group = first->zone.roundRobinGroup;
    std::uint32_t members = 0;
    for (const SamplerZone& candidate : zones) {
        members += (candidate.zone.roundRobinGroup == group && plays(candidate.zone, key, velocity))
                       ? 1
                       : 0;
    }
    const std::uint32_t turn = m_roundRobin[group]++ % members;
    // The turn-th smallest index among the matching members.
    const SamplerZone* chosen = first;
    std::uint32_t lastIndex = 0;
    bool haveLast = false;
    for (std::uint32_t t = 0; t <= turn; ++t) {
        const SamplerZone* next = nullptr;
        for (const SamplerZone& candidate : zones) {
            if (candidate.zone.roundRobinGroup != group || !plays(candidate.zone, key, velocity)) {
                continue;
            }
            const std::uint32_t index = candidate.zone.roundRobinIndex;
            if (haveLast && index <= lastIndex) {
                continue;
            }
            if (next == nullptr || index < next->zone.roundRobinIndex) {
                next = &candidate;
            }
        }
        if (next == nullptr) {
            break;
        }
        chosen = next;
        lastIndex = next->zone.roundRobinIndex;
        haveLast = true;
    }
    return chosen;
}

void SamplerInstrument::addCrossfadeLayers(SamplerVoice& state, std::uint8_t key,
                                           std::uint8_t velocity, bool byDynamics,
                                           float source) const noexcept {
    const SamplerZone* first = state.layers[0].zone;
    state.layers[0].gain = crossfadeGain(first->zone, source);
    state.layers[0].scaled = true;
    for (const SamplerZone& candidate : zones()) {
        // One slot is kept for a release zone.
        if (state.layerCount + 1 >= kSamplerLayers) {
            break;
        }
        const project::SampleZone& zone = candidate.zone;
        if (&candidate == first || !zone.hasXfade() ||
            zone.trigger != project::ZoneTrigger::Attack || !switched(zone) || key < zone.keyLow ||
            key > zone.keyHigh) {
            continue;
        }
        // By velocity only the layers the note's velocity reaches can ever sound; by
        // dynamics any of them may, once the parameter moves.
        if (!byDynamics && !zone.matches(key, velocity)) {
            continue;
        }
        state.layers[state.layerCount++] = SamplerLayer{.zone = &candidate,
                                                        .position = static_cast<double>(zone.start),
                                                        .direction = 1.0,
                                                        .gain = crossfadeGain(zone, source),
                                                        .scaled = true,
                                                        .tailStarted = false,
                                                        .active = true,
                                                        .release = false};
    }
}

void SamplerInstrument::addReleaseLayer(SamplerVoice& state,
                                        const graph::Voice& voice) const noexcept {
    if (state.layerCount >= kSamplerLayers) {
        return;
    }
    for (const SamplerZone& candidate : zones()) {
        const project::SampleZone& zone = candidate.zone;
        if (zone.trigger != project::ZoneTrigger::Release ||
            !zone.matches(voice.pitch, voice.velocity) || !switched(zone)) {
            continue;
        }
        const float held = static_cast<float>(voice.age) / static_cast<float>(sampleRate());
        state.layers[state.layerCount++] =
            SamplerLayer{.zone = &candidate,
                         .position = static_cast<double>(zone.start),
                         .direction = 1.0,
                         .gain = dsp::dbToGainF(-zone.rtDecay * held),
                         .scaled = true,
                         .tailStarted = false,
                         .active = true,
                         .release = true};
        return;
    }
}

void SamplerInstrument::chokeGroup(const graph::Voice& starting, std::uint16_t group) noexcept {
    for (const graph::Voice& other : pool().voices()) {
        if (&other == &starting || other.phase == graph::VoicePhase::Free) {
            continue;
        }
        SamplerVoice& state = stateOf(other);
        for (std::uint32_t l = 0; l < state.layerCount; ++l) {
            if (state.layers[l].zone != nullptr && state.layers[l].zone->zone.offBy == group) {
                state.choked = true;
            }
        }
    }
}

void SamplerInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                                   const graph::VoiceRender& render) noexcept {
    SamplerVoice& state = stateOf(voice);
    state.layerCount = 0;
    state.cut = 1.0F;
    state.released = false;
    state.choked = false;
    m_latest = voice.key;
    const float sensitivity = std::clamp(render.param(idx(P::Velocity)), 0.0F, 1.0F);
    const float velocity = static_cast<float>(voice.velocity) / 127.0F;
    state.velocityGain = 1.0F - sensitivity + (sensitivity * velocity);
    state.shape.attackSeconds = render.param(idx(P::EnvAttack));
    state.shape.decaySeconds = render.param(idx(P::EnvDecay));
    state.shape.sustain = render.param(idx(P::EnvSustain));
    state.shape.releaseSeconds = render.param(idx(P::EnvRelease));
    state.env.reset();
    state.env.trigger(state.shape, sampleRate());
    voice.level = 0.0F;

    // A keyswitch selects an articulation and makes no sound.
    for (const SamplerZone& candidate : zones()) {
        const project::SampleZone& zone = candidate.zone;
        if (zone.hasSwitch() && voice.pitch >= zone.switchLow && voice.pitch <= zone.switchHigh) {
            m_switch = voice.pitch;
            return;
        }
    }

    const bool byDynamics = render.param(idx(P::XfadeSource)) >= 0.5F;
    const float dynamics = std::clamp(render.param(idx(P::Dynamics)), 0.0F, 1.0F) * 127.0F;
    const SamplerZone* zone =
        choose(voice.pitch,
               byDynamics ? static_cast<std::uint8_t>(std::max(1.0F, dynamics)) : voice.velocity);
    if (zone == nullptr) {
        return;
    }
    state.layers[0] = SamplerLayer{.zone = zone,
                                   .position = static_cast<double>(zone->zone.start),
                                   .direction = 1.0,
                                   .gain = 1.0F,
                                   .scaled = false,
                                   .tailStarted = false,
                                   .active = true,
                                   .release = false};
    state.layerCount = 1;
    if (zone->zone.hasXfade()) {
        addCrossfadeLayers(state, voice.pitch, voice.velocity, byDynamics,
                           byDynamics ? dynamics : static_cast<float>(voice.velocity));
    }
    if (zone->zone.group != 0) {
        chokeGroup(voice, zone->zone.group);
    }
}

// NOLINTNEXTLINE(readability-function-size) - one voice: pitch, loop, read, envelope.
bool SamplerInstrument::renderVoice(graph::Voice& voice, std::span<float> left,
                                    std::span<float> right,
                                    const graph::VoiceRender& render) noexcept {
    SamplerVoice& state = stateOf(voice);
    if (state.layerCount == 0) {
        std::ranges::fill(left, 0.0F);
        std::ranges::fill(right, 0.0F);
        voice.level = 0.0F;
        return false; // no zone for this key: the note is silent, and done
    }
    const bool oneShot = render.param(idx(P::OneShot)) >= 0.5F;
    // A newer note chokes this one: a 2 ms fade, so the cut is not a click.
    const bool choked =
        state.choked || (render.param(kSamplerChokeIndex) >= 0.5F && !(voice.key == m_latest));
    const float cutStep = 1.0F / (0.002F * static_cast<float>(sampleRate()));

    if (voice.phase == graph::VoicePhase::Released && !state.released) {
        state.released = true;
        if (!oneShot) {
            state.env.release(state.shape, sampleRate());
        }
        for (std::uint32_t l = 0; l < state.layerCount; ++l) {
            SamplerLayer& layer = state.layers[l];
            const project::SampleZone& map = layer.zone->zone;
            if (map.loop == LoopMode::Release && !layer.tailStarted) {
                // Release mode: the held part played through; the note-off jumps to the
                // tail.
                layer.tailStarted = true;
                if (map.loopEnd != 0) {
                    layer.position = static_cast<double>(map.loopEnd);
                }
            }
        }
        addReleaseLayer(state, voice);
    }

    const bool sinc = render.param(idx(P::Interpolation)) >= 0.5F;
    const float gain = dsp::dbToGainF(render.param(idx(P::Gain)));
    const bool gainAutomated =
        idx(P::Gain) < render.automation.size() && render.automation[idx(P::Gain)] != nullptr;
    const bool fixedPitch = render.param(kSamplerFixedPitchIndex) >= 0.5F;
    const bool byDynamics = render.param(idx(P::XfadeSource)) >= 0.5F;

    // Each layer's constants for this call.
    std::array<LayerCall, kSamplerLayers> calls{};
    for (std::uint32_t l = 0; l < state.layerCount; ++l) {
        const SamplerZone& zone = *state.layers[l].zone;
        const project::SampleZone& map = zone.zone;
        LayerCall& call = calls[l];
        // Loading: play silence, but keep time, so a sample that arrives mid-note comes
        // in where it would have been rather than late.
        call.ready = zone.sample != nullptr && zone.sample->ready();
        call.view = call.ready ? zone.sample->view() : format::SampleView{};
        // A slice's end, when it has one, is the end of the sample as far as this voice
        // goes.
        std::size_t playable = call.ready ? call.view.frames : 0;
        if (map.end != 0) {
            playable = std::min<std::size_t>(playable, map.end);
        }
        call.frames = static_cast<double>(playable);
        // The fade before a slice's end: 2 ms, or a quarter of a very short slice.
        call.endFade =
            map.end != 0
                ? std::max(1.0, std::min(0.002 * static_cast<double>(call.view.sampleRate),
                                         (call.frames - static_cast<double>(map.start)) * 0.25))
                : 0.0;
        call.loopStart = static_cast<double>(map.loopStart);
        call.loopEnd = map.loopEnd != 0 ? static_cast<double>(map.loopEnd) : call.frames;
        call.loopLength = call.loopEnd - call.loopStart;
        call.rootOffset =
            (fixedPitch
                 ? 0.0F
                 : (static_cast<float>(voice.pitch) - static_cast<float>(map.rootKey)) * 100.0F) +
            map.tuneCents + render.param(idx(P::Tune));
        call.rateRatio =
            call.ready ? static_cast<double>(call.view.sampleRate) / sampleRate() : 1.0;
        call.cents = std::numeric_limits<float>::quiet_NaN();
        call.increment = 1.0;
    }

    bool alive = true;
    float env = state.env.level();
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        const float glide = i < render.pitchCents.size() ? render.pitchCents[i] : 0.0F;
        if (byDynamics && control(voice, i)) {
            // A swell: the layers' crossfade follows the parameter while the note holds.
            const float dynamics =
                std::clamp(render.paramAt(idx(P::Dynamics), i), 0.0F, 1.0F) * 127.0F;
            for (std::uint32_t l = 0; l < state.layerCount; ++l) {
                SamplerLayer& layer = state.layers[l];
                if (!layer.release && layer.zone->zone.hasXfade()) {
                    layer.gain = crossfadeGain(layer.zone->zone, dynamics);
                }
            }
        }

        std::array<float, kSamplerLayers> sampleLeft{};
        std::array<float, kSamplerLayers> sampleRight{};
        bool any = false;
        for (std::uint32_t l = 0; l < state.layerCount; ++l) {
            SamplerLayer& layer = state.layers[l];
            if (!layer.active) {
                continue;
            }
            LayerCall& call = calls[l];
            const project::SampleZone& map = layer.zone->zone;
            const float pitch = call.rootOffset + glide;
            if (pitch != call.cents) {
                call.cents = pitch;
                // Exactly 1 at the root key, untuned, at the project's rate: the fast
                // path.
                call.increment =
                    call.cents == 0.0F
                        ? call.rateRatio
                        : call.rateRatio * static_cast<double>(dsp::exp2F(call.cents / 1200.0F));
            }

            // Looping: fold the position back into the loop while the loop applies.
            call.looping = call.loopLength > 1.0 &&
                           (map.loop == LoopMode::Forward || map.loop == LoopMode::PingPong ||
                            (map.loop == LoopMode::Sustain && !state.released));
            if (call.looping && map.loop == LoopMode::PingPong) {
                if (layer.position >= call.loopEnd - 1.0) {
                    layer.position = (2.0 * (call.loopEnd - 1.0)) - layer.position;
                    layer.direction = -1.0;
                } else if (layer.position < call.loopStart && layer.direction < 0.0) {
                    layer.position = (2.0 * call.loopStart) - layer.position;
                    layer.direction = 1.0;
                }
            } else if (call.looping && layer.position >= call.loopEnd) {
                layer.position -=
                    call.loopLength *
                    dsp::floorExact((layer.position - call.loopStart) / call.loopLength);
            }

            if (!call.ready) {
                any = true;
                continue;
            }
            if (layer.position >= call.frames || layer.position < 0.0) {
                layer.active = false; // ran off the end: the sample is over
                continue;
            }
            any = true;
            const format::SampleView& view = call.view;
            sampleLeft[l] = read(view.left, layer.position, sinc, *m_sinc);
            sampleRight[l] = view.right == view.left
                                 ? sampleLeft[l]
                                 : read(view.right, layer.position, sinc, *m_sinc);
            // The loop seam's crossfade: approaching loopEnd, blend toward the audio that
            // precedes loopStart by the same distance, equal-power.
            if (call.looping && map.crossfade > 0 && map.loop != LoopMode::PingPong) {
                const double fade = std::min<double>(map.crossfade, call.loopLength);
                const double into = layer.position - (call.loopEnd - fade);
                if (into >= 0.0 && call.loopStart - (fade - into) >= 0.0) {
                    const double source = layer.position - call.loopLength;
                    const auto t = static_cast<float>(into / fade);
                    const float out = dsp::cosTurnsF(t * 0.25F);
                    const float in = dsp::sinTurnsF(t * 0.25F);
                    const float otherLeft = read(view.left, source, sinc, *m_sinc);
                    const float otherRight = view.right == view.left
                                                 ? otherLeft
                                                 : read(view.right, source, sinc, *m_sinc);
                    sampleLeft[l] = (sampleLeft[l] * out) + (otherLeft * in);
                    sampleRight[l] = (sampleRight[l] * out) + (otherRight * in);
                }
            }
        }
        if (!any) {
            alive = false;
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }

        env = oneShot ? 1.0F : state.env.next(state.shape);
        if (choked) {
            state.cut = std::max(0.0F, state.cut - cutStep);
        }
        const float gainNow =
            gainAutomated ? dsp::dbToGainF(render.paramAt(idx(P::Gain), i)) : gain;
        bool wrote = false;
        left[i] = 0.0F;
        right[i] = 0.0F;
        for (std::uint32_t l = 0; l < state.layerCount; ++l) {
            SamplerLayer& layer = state.layers[l];
            if (!layer.active) {
                continue;
            }
            const LayerCall& call = calls[l];
            float cut = state.cut;
            if (call.endFade > 0.0 && layer.position > call.frames - call.endFade) {
                cut *= static_cast<float>(
                    std::max(0.0, (call.frames - layer.position) / call.endFade));
            }
            const float level = cut * (layer.release ? 1.0F : env) * state.velocityGain * gainNow;
            float outLeft = sampleLeft[l] * level * layer.zone->gainLeft;
            float outRight = sampleRight[l] * level * layer.zone->gainRight;
            if (layer.scaled) {
                outLeft *= layer.gain;
                outRight *= layer.gain;
            }
            left[i] = wrote ? left[i] + outLeft : outLeft;
            right[i] = wrote ? right[i] + outRight : outRight;
            wrote = true;
            layer.position += call.increment * layer.direction;
        }

        if (state.cut <= 0.0F) {
            alive = false;
        } else if (!oneShot && state.env.finished()) {
            // The envelope is over: only a release zone still has something to play.
            alive = false;
            for (std::uint32_t l = 0; l < state.layerCount; ++l) {
                state.layers[l].active = state.layers[l].active && state.layers[l].release;
                alive = alive || state.layers[l].active;
            }
        }
    }
    voice.level = env * state.velocityGain;
    return alive;
}

} // namespace adx::instruments
