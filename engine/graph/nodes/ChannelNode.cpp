#include "engine/graph/nodes/ChannelNode.h"

#include <algorithm>
#include <cmath>

#include "engine/graph/nodes/Gain.h"

namespace adx::graph {
namespace {

/// Starts a straight-line glide of the voice's pitch offset to `target` cents over
/// `samples` frames. Zero frames is a jump.
void startGlide(Voice& voice, float target, std::uint32_t samples) noexcept {
    if (samples == 0) {
        voice.pitchOffset = target;
        voice.glideRemaining = 0;
        return;
    }
    voice.glideTarget = target;
    voice.glideStep = (target - voice.pitchOffset) / static_cast<float>(samples);
    voice.glideRemaining = samples;
}

} // namespace

ChannelNode::ChannelNode(std::uint32_t channelId, std::uint16_t maxPolyphony,
                         project::VoiceStealMode stealMode) noexcept
    : m_channelId(channelId), m_maxPolyphony(maxPolyphony == 0 ? 1 : maxPolyphony),
      m_stealMode(stealMode) {}

void ChannelNode::prepare(const PrepareInfo& info) {
    // Sized from the polyphony the channel declared - per channel, so a 64-voice pad
    // cannot eat the kick's voices (FINAL_PLAN §3.3.6).
    m_voiceStorage.allocate(VoicePool::storageFor(m_maxPolyphony));
    const auto fade = static_cast<std::uint32_t>(
        std::lround(kStealFadeSeconds * static_cast<double>(info.sampleRate)));
    m_pool = VoicePool{m_maxPolyphony, m_voiceStorage.view(), fade};
}

void ChannelNode::reset() noexcept {
    m_pool.clear();
    m_hasLastPitch = false;
}

PortSpec ChannelNode::ports() const noexcept {
    return PortSpec{.inputs = 0, .outputs = 1, .acceptsEvents = true};
}

float ChannelNode::portamentoSeconds(std::span<const float> /*params*/) const noexcept {
    return 0.0F;
}

std::size_t ChannelNode::voiceIndex(const Voice& voice) const noexcept {
    const std::span<const Voice> all = m_voiceStorage.view();
    return static_cast<std::size_t>(&voice - all.data());
}

void ChannelNode::apply(const BlockEvent& event, const VoiceRender& render) noexcept {
    const VoiceKey key{
        .channelId = m_channelId, .noteId = event.noteId, .instance = event.instance};
    switch (event.kind) {
    case BlockEventKind::NoteOn: {
        // Legato: another note is still held when this one starts. Checked before the
        // allocation, which may steal the very voice that makes it legato.
        bool legato = false;
        for (const Voice& voice : m_pool.voices()) {
            legato = legato || voice.phase == VoicePhase::Active;
        }
        Voice* voice = m_pool.allocate(key, m_stealMode);
        if (voice == nullptr) {
            return; // VoiceStealMode::None at the limit: the note is dropped, by design
        }
        voice->pitch = event.pitch;
        voice->velocity = event.velocity;
        voice->timeSource = event.timeSource;
        voice->endTick = event.endTick;
        // Age to the sample, not the block, so two notes in one block still have an
        // order for the stealer to use.
        voice->startedAt = m_pool.clock() + event.offset;
        voice->age = 0;
        voice->pitchOffset = 0.0F;
        voice->glideRemaining = 0;

        const float portamento = portamentoSeconds(render.params);
        if (portamento > 0.0F && legato && m_hasLastPitch && m_lastPitch != event.pitch) {
            // From the previous note's pitch to this one's: a slide in the voice's own
            // offset, so a later explicit slide simply takes over from wherever it is.
            voice->pitchOffset =
                100.0F * (static_cast<float>(m_lastPitch) - static_cast<float>(event.pitch));
            startGlide(*voice, 0.0F,
                       static_cast<std::uint32_t>(
                           std::lround(static_cast<double>(portamento) * render.sampleRate)));
        }
        m_lastPitch = event.pitch;
        m_hasLastPitch = true;
        startVoice(*voice, event, render);
        return;
    }
    case BlockEventKind::NoteOff: {
        if (Voice* voice = m_pool.find(key)) {
            VoicePool::release(*voice);
        }
        return;
    }
    case BlockEventKind::PitchGlide: {
        // A glide for a voice that is gone - stolen, or already finished - has nothing
        // to move and is dropped (phase_4.md §4.2).
        if (Voice* voice = m_pool.findSounding(key)) {
            startGlide(*voice, event.value, event.duration);
        }
        return;
    }
    case BlockEventKind::Lyric:
        // Read by the Voice instrument (§4.13) from its own event view; nothing for the
        // base to do.
        return;
    case BlockEventKind::ReleaseAll:
        m_pool.releaseAll(event.timeSource);
        return;
    case BlockEventKind::LoopWrap:
        m_pool.releaseEndingAtOrAfter(event.timeSource, event.endTick);
        return;
    }
}

void ChannelNode::renderSegment(std::span<float> left, std::span<float> right,
                                std::span<float> scratchLeft, std::span<float> scratchRight,
                                std::span<float> cents, const ProcessContext& context,
                                VoiceRender& render) noexcept {
    const auto frames = static_cast<std::uint32_t>(left.size());
    const std::span<float> voiceLeft = scratchLeft.first(frames);
    const std::span<float> voiceRight = scratchRight.first(frames);
    const std::span<float> voiceCents = cents.first(frames);
    const auto fadeTotal = static_cast<float>(m_pool.fadeSamples());
    constexpr auto kPitch = static_cast<std::uint32_t>(ChannelParam::PitchCents);

    for (Voice& voice : m_pool.voices()) {
        if (voice.phase == VoicePhase::Free) {
            continue;
        }
        // The voice's pitch, per frame: the channel's, plus the voice's own offset
        // advanced along whatever glide it is on. Advancing it here, one frame at a
        // time, is what makes a slide smooth rather than stepped at block boundaries.
        for (std::uint32_t i = 0; i < frames; ++i) {
            voiceCents[i] = paramAt(context, kPitch, render.frameOffset + i) + voice.pitchOffset;
            if (voice.glideRemaining > 0) {
                --voice.glideRemaining;
                voice.pitchOffset = voice.glideRemaining == 0 ? voice.glideTarget
                                                              : voice.pitchOffset + voice.glideStep;
            }
        }
        render.pitchCents = voiceCents;
        const bool alive = renderVoice(voice, voiceLeft, voiceRight, render);
        voice.age += frames;

        if (voice.phase == VoicePhase::Stolen) {
            // The steal fade: linear to zero over kStealFadeSeconds, then free. Applied
            // here rather than in the instrument so no instrument can forget it.
            for (std::size_t i = 0; i < frames; ++i) {
                const float gain = static_cast<float>(voice.fadeRemaining) / fadeTotal;
                voiceLeft[i] *= gain;
                voiceRight[i] *= gain;
                if (voice.fadeRemaining > 0) {
                    --voice.fadeRemaining;
                }
            }
            if (voice.fadeRemaining == 0) {
                VoicePool::free(voice);
            }
        } else if (!alive) {
            VoicePool::free(voice);
        }

        for (std::size_t i = 0; i < frames; ++i) {
            left[i] += voiceLeft[i];
            right[i] += voiceRight[i];
        }
    }
}

void ChannelNode::applyChannelStrip(const ProcessContext& context) noexcept {
    constexpr auto kVolume = static_cast<std::uint32_t>(ChannelParam::Volume);
    constexpr auto kPan = static_cast<std::uint32_t>(ChannelParam::Pan);
    constexpr auto kAudible = static_cast<std::uint32_t>(ChannelParam::Audible);
    const std::span<float> left = context.outputs[0];
    const std::span<float> right = context.outputs[1];

    if (!paramMoves(context, kVolume) && !paramMoves(context, kPan)) {
        applyGainPan(left, right, context.params[kVolume] * context.params[kAudible],
                     context.params[kPan]);
        return;
    }
    // Automated: per frame, so a volume ramp is a ramp and not a staircase at block
    // edges (P3-3).
    const float audible = context.params[kAudible];
    for (std::uint32_t i = 0; i < context.frames; ++i) {
        const PanGains sides = balance(paramAt(context, kPan, i));
        const float gain = paramAt(context, kVolume, i) * audible;
        left[i] *= gain * sides.left;
        right[i] *= gain * sides.right;
    }
}

void ChannelNode::process(ProcessContext& context) noexcept {
    const std::uint32_t frames = context.frames;
    const std::span<float> left = context.outputs[0];
    const std::span<float> right = context.outputs[1];
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);

    const std::span<float> scratchLeft = context.arena.allocate<float>(frames);
    const std::span<float> scratchRight = context.arena.allocate<float>(frames);
    const std::span<float> cents = context.arena.allocate<float>(frames);
    const bool haveScratch =
        scratchLeft.size() == frames && scratchRight.size() == frames && cents.size() == frames;

    VoiceRender render{
        .sampleRate = context.sampleRate,
        .pitchCents = {},
        .params = context.params.size() > kChannelParamCount
                      ? context.params.subspan(kChannelParamCount)
                      : std::span<const float>{},
        .automation = context.automation.size() > kChannelParamCount
                          ? context.automation.subspan(kChannelParamCount)
                          : std::span<const float* const>{},
        .frameOffset = 0,
    };

    // Dispatch: one advancing index over events already sorted by offset. Render up to
    // the next event, apply every event at that offset, repeat - O(frames + events),
    // never O(frames x events) (FINAL_PLAN §3.3.2).
    const EventView events = context.events;
    std::size_t next = 0;
    std::uint32_t position = 0;
    while (position < frames) {
        render.frameOffset = position;
        render.pitchCents = {};
        while (next < events.size() && events[next].offset <= position) {
            apply(events[next], render);
            ++next;
        }
        const std::uint32_t segmentEnd =
            next < events.size() ? std::min(events[next].offset, frames) : frames;
        if (segmentEnd > position && haveScratch) {
            const std::uint32_t length = segmentEnd - position;
            renderSegment(left.subspan(position, length), right.subspan(position, length),
                          scratchLeft, scratchRight, cents, context, render);
        }
        position = segmentEnd > position ? segmentEnd : frames;
    }
    // Anything left (an offset at or past the end cannot happen, but a bad one must
    // not be lost): apply it now so a note-off is never dropped.
    render.frameOffset = frames > 0 ? frames - 1 : 0;
    while (next < events.size()) {
        apply(events[next], render);
        ++next;
    }

    m_pool.advanceClock(frames);
    applyChannelStrip(context);
}

bool SilentChannelNode::renderVoice(Voice& voice, std::span<float> left, std::span<float> right,
                                    const VoiceRender& /*render*/) noexcept {
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);
    voice.level = 0.0F;
    return voice.phase == VoicePhase::Active;
}

} // namespace adx::graph
