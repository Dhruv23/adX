// adx-thread: realtime
#include "engine/plugin/PluginInstrument.h"

#include <algorithm>

#include "engine/project/VoiceStealMode.h"

namespace adx::plugin {

PluginInstrument::PluginInstrument(std::uint32_t channelId, std::string_view format,
                                   const project::PluginRef& ref)
    // The pool is unused - the plugin has its own voices - so it is the smallest one.
    : graph::ChannelNode(channelId, 1, project::VoiceStealMode::OldestReleased) {
    m_core.configure(format, ref, true);
}

void PluginInstrument::prepare(const graph::PrepareInfo& info) {
    graph::ChannelNode::prepare(info);
    m_core.prepare(info);
    m_events.allocate(bridge::kMaxEvents);
    m_sampleRate = info.sampleRate;
}

void PluginInstrument::push(const bridge::WireEvent& event) noexcept {
    const std::span<bridge::WireEvent> events = m_events.view();
    if (m_eventCount < events.size()) {
        events[m_eventCount++] = event;
    }
}

void PluginInstrument::releaseHeld(std::uint32_t offset, bool pastLoopEnd,
                                   std::int64_t loopEnd) noexcept {
    for (Held& held : m_held) {
        if (!held.active || (pastLoopEnd && held.endTick < loopEnd)) {
            continue;
        }
        bridge::WireEvent off{};
        off.offset = offset;
        off.noteId = static_cast<std::int32_t>(held.noteId & 0x7FFFFFFFU);
        off.kind = bridge::WireEventKind::NoteOff;
        off.key = held.key;
        push(off);
        held.active = false;
    }
}

bool PluginInstrument::renderWhole(const graph::ProcessContext& context, std::span<float> left,
                                   std::span<float> right, bool releaseAll) noexcept {
    m_eventCount = 0;
    if (releaseAll) {
        releaseHeld(0, false, 0);
        bridge::WireEvent all{};
        all.kind = bridge::WireEventKind::AllNotesOff;
        push(all);
        m_tailLeft = static_cast<std::uint64_t>(kTailSeconds * m_sampleRate);
    }
    for (const graph::BlockEvent& event : context.events) {
        switch (event.kind) {
        case graph::BlockEventKind::NoteOn: {
            const auto free = std::ranges::find_if(m_held, [](const Held& h) { return !h.active; });
            if (free != m_held.end()) {
                *free = Held{.noteId = event.noteId,
                             .instance = event.instance,
                             .endTick = event.endTick,
                             .key = event.pitch,
                             .active = true};
            }
            bridge::WireEvent on{};
            on.offset = event.offset;
            on.noteId = static_cast<std::int32_t>(event.noteId & 0x7FFFFFFFU);
            on.kind = bridge::WireEventKind::NoteOn;
            on.key = event.pitch;
            on.velocity = std::max<std::uint8_t>(event.velocity, 1);
            push(on);
            break;
        }
        case graph::BlockEventKind::NoteOff: {
            const auto match = std::ranges::find_if(m_held, [&event](const Held& h) {
                return h.active && h.noteId == event.noteId && h.instance == event.instance;
            });
            bridge::WireEvent off{};
            off.offset = event.offset;
            off.noteId = static_cast<std::int32_t>(event.noteId & 0x7FFFFFFFU);
            off.kind = bridge::WireEventKind::NoteOff;
            off.key = match != m_held.end() ? match->key : event.pitch;
            if (match != m_held.end()) {
                match->active = false;
            }
            push(off);
            break;
        }
        case graph::BlockEventKind::ReleaseAll:
            releaseHeld(event.offset, false, 0);
            break;
        case graph::BlockEventKind::LoopWrap:
            releaseHeld(event.offset, true, event.endTick);
            break;
        case graph::BlockEventKind::PitchGlide:
        case graph::BlockEventKind::Lyric:
            break; // a plugin instrument does its own pitch; lyrics are the Voice's
        }
    }
    const std::span<const float> params = context.params.size() > graph::kChannelParamCount
                                              ? context.params.subspan(graph::kChannelParamCount)
                                              : std::span<const float>{};
    const std::span<const float* const> automation =
        context.automation.size() > graph::kChannelParamCount
            ? context.automation.subspan(graph::kChannelParamCount)
            : std::span<const float* const>{};
    const std::array<float*, 2> outputs{left.data(), right.data()};
    const CoreResult result = m_core.process(
        {}, outputs, context.frames, params, automation,
        std::span<const bridge::WireEvent>(m_events.view().data(), m_eventCount), context.time);
    if (result == CoreResult::Absent) {
        std::ranges::fill(left, 0.0F);
        std::ranges::fill(right, 0.0F);
    }
    m_tailLeft -= std::min<std::uint64_t>(m_tailLeft, context.frames);
    return true;
}

} // namespace adx::plugin
