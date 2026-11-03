#include "engine/graph/EngineCore.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "engine/record/InputCapture.h"
#include "engine/rt/Violation.h"

namespace adx::graph {

bool EngineCore::post(const EngineMessage& message) noexcept {
    if (!m_queue.tryPush(message)) {
        ADX_RECORD_VIOLATION(rt::ViolationKind::Unbounded, 0);
        return false;
    }
    return true;
}

void EngineCore::install(project::Snapshot* snapshot) noexcept {
    if (snapshot == nullptr) {
        return;
    }
    if (m_current != nullptr) {
        // The retired snapshot goes to the reaper, never to delete: this is the audio
        // thread. If the reaper is full the snapshot leaks, loudly - Reaper::retire
        // records the violation.
        static_cast<void>(m_reaper.retire(m_current->lifetime));
    }
    m_current = snapshot;
    // The project's routes when it has any; otherwise every input plays the selected
    // channel. Swapped here, between blocks, with the snapshot that owns them.
    m_router.setRoutes(snapshot->midiRoutes.empty() ? std::span<const midi::MidiRoute>(m_routes)
                                                    : snapshot->midiRoutes);
    m_toggled.fill(false);
    // The arrangement follows the snapshot's tempo map. When the map really changed,
    // the source keeps its musical position and invalidates the cursors itself.
    m_transport.arrangement().bindTempo(snapshot->tempo, snapshot->sampleRate);
    m_renderedRevision.store(snapshot->revision, std::memory_order_release);
    m_swaps.fetch_add(1, std::memory_order_release);
}

void EngineCore::drain() noexcept {
    // Bounded by the ring's capacity: the loop cannot run longer than kQueueCapacity
    // iterations per block, however fast the main thread posts.
    EngineMessage message;
    for (std::size_t i = 0; i < kQueueCapacity && m_queue.tryPop(message); ++i) {
        switch (message.kind) {
        case MessageKind::Snapshot:
            install(message.snapshot);
            break;
        case MessageKind::Param:
            if (m_current != nullptr) {
                const std::uint32_t index = m_current->findParam(message.param);
                if (index < m_current->params.size()) {
                    m_current->params[index] = message.value;
                    // The base too: a knob turn on an automated parameter is what the
                    // parameter returns to where no lane covers it.
                    if (index < m_current->paramBase.size()) {
                        m_current->paramBase[index] = message.value;
                    }
                }
            }
            break;
        case MessageKind::Seek:
            m_transport.get(transport::TimeSourceId{message.timeSource})
                .requestSeek(core::Ticks{message.tickA});
            break;
        case MessageKind::State:
            m_transport.get(transport::TimeSourceId{message.timeSource})
                .requestState(message.state);
            break;
        case MessageKind::Loop:
            m_transport.get(transport::TimeSourceId{message.timeSource})
                .requestLoop(transport::LoopRegion{.start = core::Ticks{message.tickA},
                                                   .end = core::Ticks{message.tickB},
                                                   .enabled = message.flag});
            break;
        case MessageKind::Rate:
            m_transport.get(transport::TimeSourceId{message.timeSource}).requestRate(message.rate);
            break;
        case MessageKind::Audition:
            m_audition.start(
                message.param.owner,
                static_cast<std::uint8_t>(std::clamp<std::int64_t>(message.tickA, 0, 127)),
                static_cast<std::uint8_t>(std::clamp(message.value, 1.0F, 127.0F)),
                static_cast<std::uint32_t>(std::max<std::int64_t>(message.tickB, 1)));
            break;
        case MessageKind::AuditionStop:
            m_audition.releaseAll();
            break;
        case MessageKind::Metronome:
            m_metronome.setEnabled(
                message.flag, message.tickA,
                static_cast<std::uint32_t>(std::max<std::int64_t>(message.tickB, 1)));
            break;
        case MessageKind::Ping:
            m_pingAt = message.tickA;
            m_pingLevel = message.value;
            break;
        case MessageKind::MidiTarget:
            m_routes[0].target = static_cast<std::uint32_t>(message.tickA);
            break;
        }
    }
}

void EngineCore::process(float* out, const float* in, std::uint32_t frames, std::uint32_t channels,
                         rt::BlockArena& arena) noexcept {
    drain();
    if (m_current == nullptr) {
        // No project yet: silence, but the transport still answers requests and
        // publishes, so a UI polling the playhead is not left looking at stale state.
        for (std::uint32_t s = 0; s < transport::kMaxTimeSources; ++s) {
            const transport::TimeSourceId id{s};
            if (m_transport.inUse(id)) {
                static_cast<void>(m_transport.get(id).beginBlock());
                m_transport.get(id).publish();
            }
        }
        return;
    }
    m_scheduler.setAudition(gatherLive(m_audition.beginBlock(frames), frames));
    m_scheduler.render(*m_current, m_transport, arena, out, frames, channels, in, m_inputChannels);
    // The input belongs where this block started: the scheduler noted the arrangement's
    // position after the block's seek and play requests applied.
    if (record::InputCapture* capture = m_capture.load(std::memory_order_acquire)) {
        capture->capture(in, frames, m_inputChannels, m_scheduler.blockStart(),
                         m_scheduler.blockRolling());
    }
    // The click is the device's, not the mix's: after the graph, so it never reaches a
    // meter or a bounce. Positions are read after the render, which applied this block's
    // play, seek and loop requests; the block's start is its end less what it played. A
    // block that wrapped a loop places its clicks as if it had not - a click at most a
    // block off, once per wrap.
    const transport::TimeSource& arrangement = m_transport.arrangement();
    const std::int64_t toSamples = arrangement.positionSamples();
    const std::int64_t toTicks = arrangement.positionTicks().value;
    const auto played = static_cast<std::int64_t>(static_cast<double>(frames) * arrangement.rate());
    const std::int64_t fromSamples = std::max<std::int64_t>(0, toSamples - played);
    const std::int64_t fromTicks =
        arrangement.rolling() ? arrangement.tempo()
                                    .toTicks(core::Samples{fromSamples}, arrangement.sampleRate())
                                    .value
                              : toTicks;
    m_metronome.mix(arrangement, fromTicks, toTicks, fromSamples, out, frames, channels);
    if (m_pingAt >= 0 && arrangement.rolling()) {
        const std::int64_t offset = m_pingAt - m_scheduler.blockStart();
        if (offset >= 0 && std::cmp_less(offset, frames)) {
            for (std::uint32_t c = 0; c < channels; ++c) {
                out[(static_cast<std::size_t>(offset) * channels) + c] += m_pingLevel;
            }
            m_pingAt = -1;
        }
    }
}

void EngineCore::learn(const midi::MidiEvent& event) noexcept {
    midi::ControlMessage control;
    if (!midi::controlOf(event, control)) {
        return;
    }
    m_lastControl.publish(control);
    if (m_current == nullptr) {
        return;
    }
    const std::span<const midi::CompiledBinding> bindings = m_current->midiBindings;
    for (std::size_t b = 0; b < bindings.size(); ++b) {
        const midi::CompiledBinding& binding = bindings[b];
        if (!midi::matches(binding, control)) {
            continue;
        }
        bool unused = false;
        bool& toggled = b < m_toggled.size() ? m_toggled.at(b) : unused;
        float value = 0.0F;
        if (!midi::valueFor(binding, control, toggled, value)) {
            continue;
        }
        // As a knob turn: this block hears it. The main thread is told, so the project
        // (and the knob on screen) follows.
        const std::uint32_t index = m_current->findParam(binding.target);
        if (index < m_current->params.size()) {
            m_current->params[index] = value;
            if (index < m_current->paramBase.size()) {
                m_current->paramBase[index] = value;
            }
            static_cast<void>(
                m_learned.tryPush(midi::LearnedValue{.target = binding.target, .value = value}));
        }
    }
}

std::span<const AuditionEvent> EngineCore::gatherLive(std::span<const AuditionEvent> auditions,
                                                      std::uint32_t frames) noexcept {
    std::size_t count = std::min(auditions.size(), m_live.size());
    std::copy_n(auditions.begin(), count, m_live.begin());
    std::array<midi::RoutedEvent, midi::MidiRouter::kMaxFanOut> routed{};
    for (std::atomic<midi::MidiInput*> const& slot : m_midiInputs) {
        midi::MidiInput* input = slot.load(std::memory_order_acquire);
        if (input == nullptr) {
            continue;
        }
        const std::size_t got = input->drain(m_streamFrames, frames, m_midiScratch);
        for (std::size_t i = 0; i < got; ++i) {
            const midi::BlockMidi& incoming = m_midiScratch.at(i);
            learn(incoming.event);
            const std::size_t fanned = m_router.route(incoming.event, routed);
            for (std::size_t r = 0; r < fanned; ++r) {
                const midi::MidiEvent& event = routed.at(r).event;
                if (routed.at(r).target == kNone || (!event.isNoteOn() && !event.isNoteOff())) {
                    continue; // controllers and bend: MIDI learn's, later
                }
                if (count >= m_live.size()) {
                    m_liveOverflow.fetch_add(1, std::memory_order_acq_rel);
                    continue;
                }
                const bool on = event.isNoteOn();
                m_live.at(count++) = AuditionEvent{
                    .channel = routed.at(r).target,
                    .event =
                        BlockEvent{.offset = incoming.offset,
                                   .noteId = liveNoteId(event.device, event.channel(), event.data1),
                                   .instance = 0,
                                   .timeSource = kAuditionSource,
                                   .endTick = 0,
                                   .value = 0.0F,
                                   .duration = 0,
                                   .kind = on ? BlockEventKind::NoteOn : BlockEventKind::NoteOff,
                                   .pitch = event.data1,
                                   .velocity = on ? event.data2 : std::uint8_t{0}}};
            }
        }
    }
    return {m_live.data(), count};
}

void EngineCore::processStep(void* user, float* out, const float* in, std::uint32_t frames,
                             std::uint32_t channels, const audio::StreamTime& time,
                             rt::BlockArena& arena) noexcept {
    auto* core = static_cast<EngineCore*>(user);
    // Where this block is in wall time, for the MIDI inputs (MidiInput.h). steady_clock
    // is a register read on Windows (CallbackCore.cpp).
    core->m_streamFrames = time.streamFrames;
    core->m_midiClock.publish(time.streamFrames,
                              std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count());
    core->process(out, in, frames, channels, arena);
}

project::Snapshot* EngineCore::detach() noexcept {
    EngineMessage message;
    while (m_queue.tryPop(message)) {
        if (message.kind == MessageKind::Snapshot && message.snapshot != nullptr) {
            // Published but never rendered. Retire it the same way a swapped-out one
            // is, so the owner's teardown has exactly one path to follow.
            static_cast<void>(m_reaper.retire(message.snapshot->lifetime));
        }
    }
    project::Snapshot* current = m_current;
    m_current = nullptr;
    return current;
}

} // namespace adx::graph
