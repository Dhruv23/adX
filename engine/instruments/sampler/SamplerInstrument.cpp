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

} // namespace

SamplerInstrument::~SamplerInstrument() {
    destroySamplerPins(m_pins);
}

void SamplerInstrument::setZones(rt::OwnedArray<SamplerZone> zones, SamplerPins* pins) noexcept {
    m_zones = std::move(zones);
    destroySamplerPins(m_pins);
    m_pins = pins;
}

void SamplerInstrument::prepareInstrument(const graph::PrepareInfo& /*info*/) {
    m_sinc = &dsp::sincTable();
}

void SamplerInstrument::reset() noexcept {
    Instrument::reset();
    // The round-robin counters are part of the render: a reset of playback restarts
    // them, so a render from the top chooses the same zones every time.
    m_roundRobin.fill(0);
}

const SamplerZone* SamplerInstrument::choose(std::uint8_t key, std::uint8_t velocity) noexcept {
    const std::span<const SamplerZone> zones = m_zones.view();
    const SamplerZone* first = nullptr;
    for (const SamplerZone& candidate : zones) {
        if (candidate.zone.matches(key, velocity)) {
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
        members +=
            (candidate.zone.roundRobinGroup == group && candidate.zone.matches(key, velocity)) ? 1
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
            if (candidate.zone.roundRobinGroup != group || !candidate.zone.matches(key, velocity)) {
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

void SamplerInstrument::startVoice(graph::Voice& voice, const graph::BlockEvent& /*event*/,
                                   const graph::VoiceRender& render) noexcept {
    SamplerVoice& state = stateOf(voice);
    state.zone = choose(voice.pitch, voice.velocity);
    state.position = state.zone != nullptr ? static_cast<double>(state.zone->zone.start) : 0.0;
    state.direction = 1.0;
    state.released = false;
    state.tailStarted = false;
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
}

// NOLINTNEXTLINE(readability-function-size) - one voice: pitch, loop, read, envelope.
bool SamplerInstrument::renderVoice(graph::Voice& voice, std::span<float> left,
                                    std::span<float> right,
                                    const graph::VoiceRender& render) noexcept {
    SamplerVoice& state = stateOf(voice);
    if (state.zone == nullptr) {
        std::ranges::fill(left, 0.0F);
        std::ranges::fill(right, 0.0F);
        voice.level = 0.0F;
        return false; // no zone for this key: the note is silent, and done
    }
    const SamplerZone& zone = *state.zone;
    const project::SampleZone& map = zone.zone;
    const bool oneShot = render.param(idx(P::OneShot)) >= 0.5F;

    if (voice.phase == graph::VoicePhase::Released && !state.released) {
        state.released = true;
        if (!oneShot) {
            state.env.release(state.shape, sampleRate());
        }
        if (map.loop == LoopMode::Release && !state.tailStarted) {
            // Release mode: the held part played through; the note-off jumps to the tail.
            state.tailStarted = true;
            if (map.loopEnd != 0) {
                state.position = static_cast<double>(map.loopEnd);
            }
        }
    }

    // Loading: play silence, but keep time, so a sample that arrives mid-note comes in
    // where it would have been rather than late.
    const bool ready = zone.sample != nullptr && zone.sample->ready();
    const format::SampleView view = ready ? zone.sample->view() : format::SampleView{};
    const auto frames = static_cast<double>(ready ? view.frames : 0);
    const auto loopStart = static_cast<double>(map.loopStart);
    const double loopEnd = map.loopEnd != 0 ? static_cast<double>(map.loopEnd) : frames;
    const double loopLength = loopEnd - loopStart;
    const bool sinc = render.param(idx(P::Interpolation)) >= 0.5F;
    const float gain = dsp::dbToGainF(render.param(idx(P::Gain)));
    const bool gainAutomated =
        idx(P::Gain) < render.automation.size() && render.automation[idx(P::Gain)] != nullptr;
    const float rootOffset =
        ((static_cast<float>(voice.pitch) - static_cast<float>(map.rootKey)) * 100.0F) +
        map.tuneCents + render.param(idx(P::Tune));
    const double rateRatio = ready ? static_cast<double>(view.sampleRate) / sampleRate() : 1.0;

    float cents = std::numeric_limits<float>::quiet_NaN();
    double increment = 1.0;
    bool alive = true;
    float env = state.env.level();
    for (std::uint32_t i = 0; i < left.size(); ++i) {
        if (!alive) {
            left[i] = 0.0F;
            right[i] = 0.0F;
            continue;
        }
        const float pitch =
            rootOffset + (i < render.pitchCents.size() ? render.pitchCents[i] : 0.0F);
        if (pitch != cents) {
            cents = pitch;
            // Exactly 1 at the root key, untuned, at the project's rate: the fast path.
            increment = cents == 0.0F
                            ? rateRatio
                            : rateRatio * static_cast<double>(dsp::exp2F(cents / 1200.0F));
        }

        // Looping: fold the position back into the loop while the loop applies.
        const bool looping =
            loopLength > 1.0 && (map.loop == LoopMode::Forward || map.loop == LoopMode::PingPong ||
                                 (map.loop == LoopMode::Sustain && !state.released));
        if (looping && map.loop == LoopMode::PingPong) {
            if (state.position >= loopEnd - 1.0) {
                state.position = (2.0 * (loopEnd - 1.0)) - state.position;
                state.direction = -1.0;
            } else if (state.position < loopStart && state.direction < 0.0) {
                state.position = (2.0 * loopStart) - state.position;
                state.direction = 1.0;
            }
        } else if (looping && state.position >= loopEnd) {
            state.position -= loopLength * std::floor((state.position - loopStart) / loopLength);
        }

        float sampleLeft = 0.0F;
        float sampleRight = 0.0F;
        if (ready) {
            if (state.position >= frames || state.position < 0.0) {
                alive = false; // ran off the end: the sample is over
                left[i] = 0.0F;
                right[i] = 0.0F;
                continue;
            }
            sampleLeft = read(view.left, state.position, sinc, *m_sinc);
            sampleRight = view.right == view.left ? sampleLeft
                                                  : read(view.right, state.position, sinc, *m_sinc);
            // The loop seam's crossfade: approaching loopEnd, blend toward the audio that
            // precedes loopStart by the same distance, equal-power.
            if (looping && map.crossfade > 0 && map.loop != LoopMode::PingPong) {
                const double fade = std::min<double>(map.crossfade, loopLength);
                const double into = state.position - (loopEnd - fade);
                if (into >= 0.0 && loopStart - (fade - into) >= 0.0) {
                    const double source = state.position - loopLength;
                    const auto t = static_cast<float>(into / fade);
                    const float out = dsp::cosTurnsF(t * 0.25F);
                    const float in = dsp::sinTurnsF(t * 0.25F);
                    const float otherLeft = read(view.left, source, sinc, *m_sinc);
                    const float otherRight = view.right == view.left
                                                 ? otherLeft
                                                 : read(view.right, source, sinc, *m_sinc);
                    sampleLeft = (sampleLeft * out) + (otherLeft * in);
                    sampleRight = (sampleRight * out) + (otherRight * in);
                }
            }
        }

        env = oneShot ? 1.0F : state.env.next(state.shape);
        const float level =
            env * state.velocityGain *
            (gainAutomated ? dsp::dbToGainF(render.paramAt(idx(P::Gain), i)) : gain);
        left[i] = sampleLeft * level * zone.gainLeft;
        right[i] = sampleRight * level * zone.gainRight;

        state.position += increment * state.direction;
        if (!oneShot && state.env.finished()) {
            alive = false;
        }
    }
    voice.level = env * state.velocityGain;
    return alive;
}

} // namespace adx::instruments
