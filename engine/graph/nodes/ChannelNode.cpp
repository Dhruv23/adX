#include "engine/graph/nodes/ChannelNode.h"

#include <algorithm>
#include <cmath>

#include "engine/graph/nodes/Gain.h"

namespace adx::graph {

ChannelNode::ChannelNode(std::uint32_t channelId, std::uint16_t maxPolyphony,
                         project::VoiceStealMode stealMode) noexcept
    : m_channelId(channelId), m_maxPolyphony(maxPolyphony == 0 ? 1 : maxPolyphony),
      m_stealMode(stealMode) {}

void ChannelNode::prepare(const PrepareInfo& info) {
    // The one allocation this node ever makes, sized from the polyphony the channel
    // declared - per channel, so a 64-voice pad cannot eat the kick's voices
    // (FINAL_PLAN §3.3.6).
    m_voiceStorage.allocate(VoicePool::storageFor(m_maxPolyphony));
    const auto fade = static_cast<std::uint32_t>(
        std::lround(kStealFadeSeconds * static_cast<double>(info.sampleRate)));
    m_pool = VoicePool{m_maxPolyphony, m_voiceStorage.view(), fade};
}

void ChannelNode::reset() noexcept {
    m_pool.clear();
}

PortSpec ChannelNode::ports() const noexcept {
    return PortSpec{.inputs = 0, .outputs = 1, .acceptsEvents = true};
}

void ChannelNode::apply(const BlockEvent& event, std::uint32_t sampleRate) noexcept {
    const VoiceKey key{
        .channelId = m_channelId, .noteId = event.noteId, .instance = event.instance};
    switch (event.kind) {
    case BlockEventKind::NoteOn: {
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
        startVoice(*voice, event, sampleRate);
        return;
    }
    case BlockEventKind::NoteOff: {
        if (Voice* voice = m_pool.find(key)) {
            VoicePool::release(*voice);
        }
        return;
    }
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
                                std::uint32_t sampleRate, float pitchCents) noexcept {
    const std::size_t frames = left.size();
    const std::span<float> voiceLeft = scratchLeft.first(frames);
    const std::span<float> voiceRight = scratchRight.first(frames);
    const auto fadeTotal = static_cast<float>(m_pool.fadeSamples());

    for (Voice& voice : m_pool.voices()) {
        if (voice.phase == VoicePhase::Free) {
            continue;
        }
        const bool alive = renderVoice(voice, voiceLeft, voiceRight, sampleRate, pitchCents);

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

void ChannelNode::process(ProcessContext& context) noexcept {
    const std::uint32_t frames = context.frames;
    const std::span<float> left = context.outputs[0];
    const std::span<float> right = context.outputs[1];
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);

    const float volume = context.params[static_cast<std::uint32_t>(ChannelParam::Volume)];
    const float pan = context.params[static_cast<std::uint32_t>(ChannelParam::Pan)];
    const float audible = context.params[static_cast<std::uint32_t>(ChannelParam::Audible)];
    const float pitchCents = context.params[static_cast<std::uint32_t>(ChannelParam::PitchCents)];

    const std::span<float> scratchLeft = context.arena.allocate<float>(frames);
    const std::span<float> scratchRight = context.arena.allocate<float>(frames);
    const bool haveScratch = scratchLeft.size() == frames && scratchRight.size() == frames;

    // Dispatch: one advancing index over events already sorted by offset. Render up to
    // the next event, apply every event at that offset, repeat - O(frames + events),
    // never O(frames x events) (FINAL_PLAN §3.3.2).
    const EventView events = context.events;
    std::size_t next = 0;
    std::uint32_t position = 0;
    while (position < frames) {
        while (next < events.size() && events[next].offset <= position) {
            apply(events[next], context.sampleRate);
            ++next;
        }
        const std::uint32_t segmentEnd =
            next < events.size() ? std::min(events[next].offset, frames) : frames;
        if (segmentEnd > position && haveScratch) {
            const std::uint32_t length = segmentEnd - position;
            renderSegment(left.subspan(position, length), right.subspan(position, length),
                          scratchLeft, scratchRight, context.sampleRate, pitchCents);
        }
        position = segmentEnd > position ? segmentEnd : frames;
    }
    // Anything left (an offset at or past the end cannot happen, but a bad one must
    // not be lost): apply it now so a note-off is never dropped.
    while (next < events.size()) {
        apply(events[next], context.sampleRate);
        ++next;
    }

    m_pool.advanceClock(frames);
    applyGainPan(left, right, volume * audible, pan);
}

} // namespace adx::graph
