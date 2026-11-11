#include "engine/instruments/mod/ModLayer.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"
#include "engine/effects/GrossBeat.h"
#include "engine/transport/TimeSource.h"

namespace adx::instruments {
namespace {

using dsp::ModDest;
using dsp::ModSource;

[[nodiscard]] int choiceAt(const graph::VoiceRender& render, std::uint32_t index,
                           std::uint32_t frame, int highest) noexcept {
    return std::clamp(static_cast<int>(dsp::floorExact(render.paramAt(index, frame) + 0.5F)), 0,
                      highest);
}

/// The VA's LfoTarget order (None, Pitch, Cutoff, Amplitude, Pan, PulseWidth) as a
/// destination, and the amount that makes a depth of 1 what it has always been:
/// +-12 semitones, +-4 octaves, a full tremolo, hard left to right, +-0.45 of width.
struct LegacyRoute {
    ModDest dest;
    float scale;
};
constexpr std::array<LegacyRoute, 6> kLegacyRoutes{{{ModDest::None, 0.0F},
                                                    {ModDest::Pitch, 0.25F},
                                                    {ModDest::Cutoff, 0.5F},
                                                    {ModDest::Level, 1.0F},
                                                    {ModDest::Pan, 1.0F},
                                                    {ModDest::PulseWidth, 1.0F}}};

void readShapes(ModVoice& state, const graph::VoiceRender& render, const ModLayout& layout,
                std::uint32_t frame) noexcept {
    const std::array<ModParam, 2> first{ModParam::ModEnv1Attack, ModParam::ModEnv2Attack};
    for (std::size_t e = 0; e < 2; ++e) {
        const std::uint32_t at = layout.at(first[e]);
        state.shape[e].attackSeconds = render.paramAt(at, frame);
        state.shape[e].decaySeconds = render.paramAt(at + 1, frame);
        state.shape[e].sustain = render.paramAt(at + 2, frame);
        state.shape[e].releaseSeconds = render.paramAt(at + 3, frame);
    }
}

/// A synced LFO's turns per frame at this point of the timeline: the beats the next
/// control interval covers, over the LFO's length. Read from the tempo map's exact
/// beat position, so the LFO keeps its place through a tempo ramp.
[[nodiscard]] float syncedIncrement(const graph::VoiceRender& render, std::uint32_t frame,
                                    std::uint8_t sync) noexcept {
    const double lengthBeats = 4.0 * dsp::kLfoSyncWholes.at(sync);
    const auto rate = static_cast<double>(render.sampleRate);
    const double interval = static_cast<double>(project::kControlFrames);
    double beats = 2.0 * interval / rate; // 120 bpm when there is no timeline
    if (render.time != nullptr) {
        const double seconds =
            static_cast<double>(render.time->positionSamples() +
                                static_cast<std::int64_t>(render.frameOffset + frame)) /
            rate;
        const core::TempoView& tempo = render.time->tempo();
        beats = effects::beatsAt(tempo, seconds + (interval / rate)) -
                effects::beatsAt(tempo, seconds);
    }
    return static_cast<float>(beats / (interval * lengthBeats));
}

/// Everything read on the control grid: the envelope shapes, the slots that do
/// something, the LFOs' delay, fade and sync, and the wheel.
void readControl(ModVoice& state, const graph::VoiceRender& render, const ModLayout& layout,
                 std::uint32_t frame) noexcept {
    readShapes(state, render, layout, frame);
    const auto rate = static_cast<float>(render.sampleRate);
    const std::array<ModParam, 2> sync{ModParam::Lfo1Sync, ModParam::Lfo2Sync};
    for (std::size_t l = 0; l < 2; ++l) {
        const std::uint32_t at = layout.at(sync[l]);
        state.lfoSync[l] = static_cast<std::uint8_t>(choiceAt(render, at, frame, 12));
        state.lfoDelay[l] = std::max(0.0F, render.paramAt(at + 1, frame)) * rate;
        state.lfoFade[l] = std::max(0.0F, render.paramAt(at + 2, frame)) * rate;
        if (state.lfoSync[l] != 0) {
            state.lfoIncrement[l] = syncedIncrement(render, frame, state.lfoSync[l]);
        }
    }
    state.wheel = std::clamp(render.paramAt(layout.at(ModParam::ModWheel), frame), 0.0F, 1.0F);
    state.pressure =
        std::clamp(render.paramAt(layout.at(ModParam::Aftertouch), frame), 0.0F, 1.0F);

    state.slotCount = 0;
    state.usesEnvelopes = false;
    for (std::uint32_t s = 0; s < dsp::kModSlots; ++s) {
        const std::uint32_t at = layout.at(ModParam::Slot1Source) + (3 * s);
        const auto source =
            static_cast<ModSource>(choiceAt(render, at, frame, dsp::kModSourceCount - 1));
        const auto dest =
            static_cast<ModDest>(choiceAt(render, at + 1, frame, dsp::kModDestCount - 1));
        const float amount = std::clamp(render.paramAt(at + 2, frame), -1.0F, 1.0F);
        if (source == ModSource::None || dest == ModDest::None || amount == 0.0F) {
            continue;
        }
        state.slots[state.slotCount++] = dsp::ModSlot{.source = source, .dest = dest, .amount = amount};
        state.usesEnvelopes = state.usesEnvelopes || source == ModSource::ModEnv1 ||
                              source == ModSource::ModEnv2;
    }
}

void startPitchEnvelope(ModVoice& state, const graph::VoiceRender& render,
                        const ModLayout& layout) noexcept {
    state.pitchAmount = render.param(layout.at(ModParam::PitchEnvAmount));
    state.pitchOffset = static_cast<double>(state.pitchAmount);
    const double frames =
        std::max(1.0, static_cast<double>(render.param(layout.at(ModParam::PitchEnvTime))) *
                          static_cast<double>(render.sampleRate));
    if (choiceAt(render, layout.at(ModParam::PitchEnvCurve), 0, 1) == 1) {
        state.pitchFactor = 1.0;
        state.pitchStep = std::abs(state.pitchOffset) / frames;
    } else {
        // 1 % of the way left after `frames`: 0.01^(1 / frames) per frame.
        state.pitchFactor = dsp::exp2(dsp::log2(0.01) / frames);
        state.pitchStep = 0.0;
    }
}

/// The pitch envelope's offset for this frame, in semitones, then one frame on.
[[nodiscard]] float nextPitchOffset(ModVoice& state) noexcept {
    const auto now = static_cast<float>(state.pitchOffset);
    if (state.pitchStep > 0.0) {
        const double left = std::abs(state.pitchOffset) - state.pitchStep;
        state.pitchOffset = left <= 0.0 ? 0.0 : std::copysign(left, state.pitchOffset);
    } else {
        state.pitchOffset *= state.pitchFactor;
        // A thousandth of a cent: inaudible, and the envelope is over.
        if (std::abs(state.pitchOffset) < 1e-5) {
            state.pitchOffset = 0.0;
        }
    }
    return now;
}

struct Sources {
    std::array<float, 2> lfo;
    std::array<float, 2> env;
    float pitchEnv;
};

[[nodiscard]] float valueOf(const ModVoice& state, const Sources& sources,
                            ModSource source) noexcept {
    switch (source) {
    case ModSource::ModEnv1:
        return sources.env[0];
    case ModSource::ModEnv2:
        return sources.env[1];
    case ModSource::Lfo1:
        return sources.lfo[0];
    case ModSource::Lfo2:
        return sources.lfo[1];
    case ModSource::Velocity:
        return state.velocity;
    case ModSource::Key:
        return state.key;
    case ModSource::ModWheel:
        return state.wheel;
    case ModSource::Aftertouch:
        return state.pressure;
    case ModSource::Random:
        return state.random;
    case ModSource::PitchEnv:
        return sources.pitchEnv;
    case ModSource::None:
        break;
    }
    return 0.0F;
}

} // namespace

void modStart(ModVoice& state, const graph::Voice& voice, const graph::VoiceRender& render,
              const ModLayout& layout, std::uint32_t seed, double clockSeconds) noexcept {
    state.velocity = static_cast<float>(voice.velocity) / 127.0F;
    state.key = std::clamp((static_cast<float>(voice.pitch) - 60.0F) / 60.0F, -1.0F, 1.0F);
    // The seed's bits as -1..1: the same note, the same value, on every render.
    state.random =
        (static_cast<float>((seed * 2654435761U) >> 8U) * (2.0F / 16777216.0F)) - 1.0F;
    state.released = false;
    for (std::uint32_t l = 0; l < 2; ++l) {
        // An LFO in free mode keeps the channel's time, so two notes see one LFO;
        // retrig starts it from zero at the note. The channel's clock, never the wall
        // clock.
        state.lfo[l] = dsp::Lfo{seed ^ (l + 1)};
        const bool retrig = layout.lfoRetrig[l] == project::kNoParam ||
                            render.param(layout.lfoRetrig[l]) >= 0.5F;
        if (retrig) {
            state.lfo[l].reset(0.0F);
        } else {
            const double cycles =
                clockSeconds * static_cast<double>(render.param(layout.lfoRate[l]));
            state.lfo[l].reset(static_cast<float>(cycles - dsp::floorExact(cycles)));
        }
        state.lfoIncrement[l] = 0.0F;
        state.lfoShape[l] = dsp::LfoShape::Sine;
    }
    for (dsp::Envelope& envelope : state.env) {
        envelope.reset();
    }
    readControl(state, render, layout, 0);
    for (std::size_t e = 0; e < 2; ++e) {
        state.env[e].trigger(state.shape[e], render.sampleRate);
    }
    startPitchEnvelope(state, render, layout);
}

void modRetrigger(ModVoice& state, const graph::VoiceRender& render,
                  const ModLayout& layout) noexcept {
    state.released = false;
    readControl(state, render, layout, 0);
    for (std::size_t e = 0; e < 2; ++e) {
        state.env[e].trigger(state.shape[e], render.sampleRate);
    }
    startPitchEnvelope(state, render, layout);
}

void modBegin(ModVoice& state, const graph::Voice& voice, const graph::VoiceRender& render,
              const ModLayout& layout) noexcept {
    if (voice.phase == graph::VoicePhase::Released && !state.released) {
        state.released = true;
        for (std::size_t e = 0; e < 2; ++e) {
            state.env[e].release(state.shape[e], render.sampleRate);
        }
    }
    const float inverseRate = 1.0F / static_cast<float>(render.sampleRate);
    for (std::size_t l = 0; l < 2; ++l) {
        if (state.lfoSync[l] == 0) {
            state.lfoIncrement[l] = render.param(layout.lfoRate[l]) * inverseRate;
        }
        state.lfoShape[l] = static_cast<dsp::LfoShape>(
            choiceAt(render, layout.lfoShape[l], 0, dsp::kLfoShapeCount - 1));
    }
}

const dsp::ModFrame& modTick(ModVoice& state, const graph::Voice& voice,
                             const graph::VoiceRender& render, const ModLayout& layout,
                             std::uint32_t frame, bool control,
                             dsp::ModFrame& scratch) noexcept {
    if (control) {
        readControl(state, render, layout, frame);
    }
    Sources sources{};
    for (std::size_t l = 0; l < 2; ++l) {
        float value = state.lfo[l].next(state.lfoShape[l], state.lfoIncrement[l]);
        if (state.lfoDelay[l] > 0.0F || state.lfoFade[l] > 0.0F) {
            const float since = static_cast<float>(voice.age + frame) - state.lfoDelay[l];
            if (since < 0.0F) {
                value = 0.0F;
            } else if (since < state.lfoFade[l]) {
                value *= since / state.lfoFade[l];
            }
        }
        sources.lfo[l] = value;
    }

    // The VA's own two routings, read per frame as they always were.
    std::array<LegacyRoute, 2> legacy{};
    std::array<float, 2> legacyDepth{};
    bool routed = state.slotCount > 0 || state.pitchOffset != 0.0;
    for (std::size_t l = 0; l < 2; ++l) {
        if (layout.legacyTarget[l] == project::kNoParam) {
            continue;
        }
        legacy[l] = kLegacyRoutes.at(
            static_cast<std::size_t>(choiceAt(render, layout.legacyTarget[l], frame, 5)));
        legacyDepth[l] = std::clamp(render.paramAt(layout.legacyDepth[l], frame), 0.0F, 1.0F);
        routed = routed || legacy[l].dest != ModDest::None;
    }
    if (!routed) {
        return dsp::neutralModFrame();
    }

    scratch.clear();
    for (std::size_t l = 0; l < 2; ++l) {
        if (legacy[l].dest != ModDest::None) {
            scratch.add(legacy[l].dest, sources.lfo[l], legacyDepth[l] * legacy[l].scale);
        }
    }
    if (state.usesEnvelopes) {
        sources.env[0] = state.env[0].next(state.shape[0]);
        sources.env[1] = state.env[1].next(state.shape[1]);
    }
    if (state.pitchOffset != 0.0) {
        const float offset = nextPitchOffset(state);
        scratch.value[static_cast<std::size_t>(ModDest::Pitch)] += offset;
        sources.pitchEnv = state.pitchAmount != 0.0F ? offset / state.pitchAmount : 0.0F;
    }
    for (std::uint32_t s = 0; s < state.slotCount; ++s) {
        const dsp::ModSlot& slot = state.slots[s];
        scratch.add(slot.dest, valueOf(state, sources, slot.source), slot.amount);
    }
    return scratch;
}

VoiceMode modVoiceMode(std::span<const float> params, const ModLayout& layout) noexcept {
    const std::uint32_t at = layout.at(ModParam::VoiceMode);
    if (at >= params.size()) {
        return VoiceMode::Poly;
    }
    return static_cast<VoiceMode>(
        std::clamp(static_cast<int>(dsp::floorExact(params[at] + 0.5F)), 0, 2));
}

bool modGlideAlways(std::span<const float> params, const ModLayout& layout) noexcept {
    const std::uint32_t at = layout.at(ModParam::GlideMode);
    return at < params.size() && params[at] >= 0.5F;
}

} // namespace adx::instruments
