#include "engine/graph/EngineCore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    if (snapshot->sampleRate != m_followRate && snapshot->sampleRate > 0) {
        m_followRate = snapshot->sampleRate;
        m_clockFollower = midi::ClockFollower(m_followRate);
    }
    m_router.setRoutes(snapshot->midiRoutes.empty() ? std::span<const midi::MidiRoute>(m_routes)
                                                    : snapshot->midiRoutes);
    m_toggled.fill(false);
    // The arrangement follows the snapshot's tempo map. When the map really changed,
    // the source keeps its musical position and invalidates the cursors itself.
    m_transport.arrangement().bindTempo(snapshot->tempo, snapshot->sampleRate);
    // Running clips follow their cells into the new snapshot before the old one retires.
    m_session.rebind(*snapshot, m_transport);
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
        case MessageKind::Session:
            m_session.request(session::SessionRequest{
                .kind = static_cast<session::SessionRequest::Kind>(
                    std::clamp<std::int64_t>(message.tickA, 0, 6)),
                .track =
                    static_cast<std::uint32_t>(static_cast<std::uint64_t>(message.tickB) >> 32U),
                .row = static_cast<std::uint32_t>(static_cast<std::uint64_t>(message.tickB) &
                                                  0xFFFFFFFFU),
                .gain = message.value,
                .value = message.rate});
            break;
        case MessageKind::SyncFollow:
            m_follow = static_cast<SyncFollow>(std::clamp<std::int64_t>(message.tickA, 0, 2));
            m_mtcWasRunning = false;
            if (m_follow == SyncFollow::None) {
                m_transport.arrangement().requestRate(1.0); // back to our own clock
            }
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
    follow();
    m_scheduler.setSessionLanes(m_session.beforeRender(*m_current, m_transport, frames));
    m_scheduler.render(*m_current, m_transport, arena, out, frames, channels, in, m_inputChannels);
    m_session.afterRender(*m_current, m_transport, frames, m_scheduler.sounding());
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
    logTake();
    syncOut(midi::SyncBlock{.fromTicks = fromTicks,
                            .toTicks = toTicks,
                            .fromSamples = fromSamples,
                            .toSamples = toSamples,
                            .frames = frames,
                            .sampleRate = arrangement.sampleRate(),
                            .rate = arrangement.rate(),
                            .rolling = arrangement.rolling(),
                            .tempo = arrangement.tempo()});
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

bool EngineCore::padPress(const midi::MidiEvent& event) noexcept {
    if (event.device >= m_pads.size()) {
        return false;
    }
    const midi::PadMap* pads = m_pads.at(event.device).load(std::memory_order_acquire);
    if (pads == nullptr) {
        return false;
    }
    const midi::PadAction action = pads->actionFor(event.status, event.data1, event.data2);
    const float gain = pads->velocitySensitive && (event.status & 0xF0U) == 0x90U
                           ? pads->gain.at(event.data2 & 0x7FU)
                           : 0.0F;
    using Kind = midi::PadAction::Kind;
    using Request = session::SessionRequest;
    switch (action.kind) {
    case Kind::Cell:
        m_session.request(Request{
            .kind = Request::Kind::Launch, .track = action.track, .row = action.row, .gain = gain});
        break;
    case Kind::Scene:
        m_session.request(Request{.kind = Request::Kind::Scene, .row = action.row, .gain = gain});
        break;
    case Kind::StopTrack:
        m_session.request(Request{.kind = Request::Kind::Stop, .track = action.track});
        break;
    case Kind::StopAll:
        m_session.request(Request{.kind = Request::Kind::StopAll});
        break;
    case Kind::TapTempo:
        m_session.request(Request{.kind = Request::Kind::Tap, .value = -1.0});
        break;
    case Kind::None:
        break;
    }
    if (action.kind != Kind::None) {
        m_padPresses.fetch_add(1, std::memory_order_acq_rel);
    }
    return action.kind != Kind::None || pads->consumeAll;
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

float EngineCore::bendCents(const midi::MidiEvent& event) const noexcept {
    const LiveBend& bend =
        m_bends.at(std::min<std::size_t>(event.device, kMaxMidiInputs - 1)).at(event.channel());
    const int value = event.data1 | (static_cast<int>(event.data2) << 7);
    return static_cast<float>(value - 8192) / 8192.0F * bend.semitones * 100.0F;
}

void EngineCore::liveController(const midi::MidiEvent& event) noexcept {
    LiveBend& bend =
        m_bends.at(std::min<std::size_t>(event.device, kMaxMidiInputs - 1)).at(event.channel());
    if (event.kind() == midi::MidiKind::PitchBend) {
        bend.cents = bendCents(event);
        return;
    }
    if (event.kind() != midi::MidiKind::ControlChange) {
        return;
    }
    if (event.data1 == 101) {
        bend.rpn = static_cast<std::uint16_t>((event.data2 << 7U) | (bend.rpn & 0x7FU));
    } else if (event.data1 == 100) {
        bend.rpn = static_cast<std::uint16_t>((bend.rpn & ~0x7FU) | event.data2);
    } else if (event.data1 == 6 && bend.rpn == 0) {
        bend.semitones = event.data2;
    } else if (event.data1 == 38 && bend.rpn == 0) {
        bend.semitones = std::floor(bend.semitones) + (static_cast<float>(event.data2) / 100.0F);
    } else if (event.data1 == 6 && bend.rpn == 6 && event.data2 > 0) {
        // An MPE zone: its member channels bend 48 semitones unless told otherwise.
        for (LiveBend& member :
             m_bends.at(std::min<std::size_t>(event.device, kMaxMidiInputs - 1))) {
            member.semitones = 48.0F;
        }
    }
}

void EngineCore::logTake() noexcept {
    const std::size_t count = std::exchange(m_blockRawCount, 0);
    if (!m_recordMidi.load(std::memory_order_acquire) || !m_scheduler.blockRolling()) {
        return;
    }
    const transport::TimeSource& arrangement = m_transport.arrangement();
    for (std::size_t i = 0; i < count; ++i) {
        const midi::BlockMidi& raw = m_blockRaw.at(i);
        const auto at =
            m_scheduler.blockStart() +
            static_cast<std::int64_t>(static_cast<double>(raw.offset) * arrangement.rate());
        const std::int64_t tick =
            arrangement.tempo().toTicks(core::Samples{at}, arrangement.sampleRate()).value;
        if (!m_take.tryPush(midi::TakeEvent{.tick = tick,
                                            .status = raw.event.status,
                                            .data1 = raw.event.data1,
                                            .data2 = raw.event.data2,
                                            .device = raw.event.device})) {
            m_takeLost.fetch_add(1, std::memory_order_acq_rel);
        }
    }
}

void EngineCore::syncIn(const midi::MidiEvent& event) noexcept {
    transport::TimeSource& arrangement = m_transport.arrangement();
    if (m_follow == SyncFollow::Clock) {
        switch (event.status) {
        case midi::kClockPulse:
            m_clockFollower.pulse(event.timestampSamples);
            break;
        case midi::kClockStart:
            m_clockFollower.start();
            arrangement.requestSeek(core::Ticks{0});
            arrangement.requestState(transport::PlayState::Playing);
            break;
        case midi::kClockContinue:
            m_clockFollower.resume();
            arrangement.requestState(transport::PlayState::Playing);
            break;
        case midi::kClockStop:
            m_clockFollower.stop();
            arrangement.requestState(transport::PlayState::Stopped);
            break;
        case midi::kSongPosition: {
            const auto sixteenths = static_cast<std::uint16_t>(
                event.data1 | (static_cast<unsigned>(event.data2) << 7U));
            m_clockFollower.songPosition(sixteenths);
            arrangement.requestSeek(core::Ticks{m_clockFollower.position().value});
            break;
        }
        default:
            break;
        }
    } else if (m_follow == SyncFollow::Mtc && event.status == midi::kMtcQuarterFrame) {
        m_mtcFollower.quarter(event.data1, event.timestampSamples, m_followRate);
    }
}

void EngineCore::follow() noexcept {
    transport::TimeSource& arrangement = m_transport.arrangement();
    const std::uint32_t rate = arrangement.sampleRate();
    if (m_follow == SyncFollow::Clock) {
        if (!m_clockFollower.locked() || !m_clockFollower.running()) {
            return;
        }
        const double ours = arrangement.tempo().bpmAt(arrangement.positionTicks());
        if (ours <= 0.0) {
            return;
        }
        // Where the clock is now: its last pulse's tick plus the part of a pulse since.
        const double period = m_clockFollower.periodSamples();
        const double since =
            period > 0.0
                ? std::clamp((static_cast<double>(m_streamFrames) - m_clockFollower.lastPulse()) /
                                 period,
                             0.0, 1.0)
                : 0.0;
        const double theirs = static_cast<double>(m_clockFollower.position().value) +
                              (since * static_cast<double>(midi::kTicksPerPulse));
        const double error = theirs - static_cast<double>(arrangement.positionTicks().value);
        if (std::abs(error) > static_cast<double>(core::kPpq)) {
            arrangement.requestSeek(core::Ticks{static_cast<std::int64_t>(theirs)});
            return;
        }
        // The tempo ratio, nudged toward the clock's phase: a tenth of a beat's error is
        // a 1 % correction, capped at 2 %.
        const double nudge = std::clamp(0.1 * error / static_cast<double>(core::kPpq), -0.02, 0.02);
        arrangement.requestRate((m_clockFollower.bpm() / ours) * (1.0 + nudge));
    } else if (m_follow == SyncFollow::Mtc) {
        const bool running = m_mtcFollower.running(m_streamFrames, rate);
        if (!running) {
            if (m_mtcWasRunning) {
                arrangement.requestState(transport::PlayState::Stopped);
            }
            m_mtcWasRunning = false;
            return;
        }
        const double target = m_mtcFollower.secondsAt(m_streamFrames, rate) * rate;
        const double error = target - static_cast<double>(arrangement.positionSamples());
        if (!m_mtcWasRunning || std::abs(error) > static_cast<double>(rate) / 15.0) {
            // Two frames out, or just started: chase.
            arrangement.requestSeek(arrangement.tempo().toTicks(
                core::Samples{static_cast<std::int64_t>(std::max(0.0, target))}, rate));
            arrangement.requestState(transport::PlayState::Playing);
        }
        const double nudge = std::clamp(0.5 * error / static_cast<double>(rate), -0.02, 0.02);
        arrangement.requestRate(m_mtcFollower.rate() * (1.0 + nudge));
        m_mtcWasRunning = true;
    }
}

void EngineCore::syncOut(const midi::SyncBlock& block) noexcept {
    // The senders keep state between blocks, so they run whether or not anyone listens;
    // their messages are only queued for the outputs that asked.
    const std::size_t clocks = m_clockSender.block(block, m_syncScratch);
    const std::size_t codes =
        m_mtcSender.block(block, std::span<midi::SyncMessage>(m_syncScratch).subspan(clocks));
    for (std::atomic<midi::MidiOutput*> const& slot : m_midiOutputs) {
        midi::MidiOutput* output = slot.load(std::memory_order_acquire);
        if (output == nullptr) {
            continue;
        }
        const bool wantsClock = output->sync().clock.load(std::memory_order_relaxed);
        const bool wantsMtc = output->sync().mtc.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < clocks + codes; ++i) {
            if ((i < clocks && !wantsClock) || (i >= clocks && !wantsMtc)) {
                continue;
            }
            const midi::SyncMessage& message = m_syncScratch.at(i);
            static_cast<void>(
                output->push(midi::OutMessage{.streamFrame = m_streamFrames + message.offset,
                                              .bytes = message.bytes,
                                              .size = message.size}));
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
            if (incoming.event.kind() == midi::MidiKind::System) {
                syncIn(incoming.event);
                continue;
            }
            if (m_blockRawCount < m_blockRaw.size()) {
                m_blockRaw.at(m_blockRawCount++) = incoming;
            }
            learn(incoming.event);
            if (padPress(incoming.event)) {
                continue; // the pads' (phase_11.md §4.7): it launches, it does not play
            }
            liveController(incoming.event);
            const std::size_t fanned = m_router.route(incoming.event, routed);
            for (std::size_t r = 0; r < fanned; ++r) {
                const midi::MidiEvent& event = routed.at(r).event;
                const std::uint32_t target = routed.at(r).target;
                if (target == kNone) {
                    continue;
                }
                const auto glide = [&](std::uint32_t noteId, float cents) {
                    if (count >= m_live.size()) {
                        m_liveOverflow.fetch_add(1, std::memory_order_acq_rel);
                        return;
                    }
                    m_live.at(count++) =
                        AuditionEvent{.channel = target,
                                      .event = BlockEvent{.offset = incoming.offset,
                                                          .noteId = noteId,
                                                          .instance = 0,
                                                          .timeSource = kAuditionSource,
                                                          .endTick = 0,
                                                          .value = cents,
                                                          .duration = 0,
                                                          .kind = BlockEventKind::PitchGlide,
                                                          .pitch = 0,
                                                          .velocity = 0,
                                                          .part = 0}};
                };
                if (event.kind() == midi::MidiKind::PitchBend) {
                    // Every note this input channel holds on this target bends with it.
                    const float cents = bendCents(event);
                    for (const HeldLive& held : m_held) {
                        if (held.active && held.target == target && held.device == event.device &&
                            held.channel == event.channel()) {
                            glide(held.noteId, cents);
                        }
                    }
                    continue;
                }
                if (event.kind() == midi::MidiKind::ControlChange ||
                    event.kind() == midi::MidiKind::ProgramChange) {
                    // Also the channel's own: a gm part's program, bank and mix.
                    if (count >= m_live.size()) {
                        m_liveOverflow.fetch_add(1, std::memory_order_acq_rel);
                        continue;
                    }
                    const bool program = event.kind() == midi::MidiKind::ProgramChange;
                    m_live.at(count++) = AuditionEvent{
                        .channel = target,
                        .event = BlockEvent{.offset = incoming.offset,
                                            .noteId = 0,
                                            .instance = 0,
                                            .timeSource = kAuditionSource,
                                            .endTick = 0,
                                            .value = 0.0F,
                                            .duration = 0,
                                            .kind = BlockEventKind::Controller,
                                            .pitch = program ? kProgramChange : event.data1,
                                            .velocity = program ? event.data1 : event.data2,
                                            .part = event.channel()}};
                    continue;
                }
                if (!event.isNoteOn() && !event.isNoteOff()) {
                    continue; // other controllers: MIDI learn's
                }
                if (count >= m_live.size()) {
                    m_liveOverflow.fetch_add(1, std::memory_order_acq_rel);
                    continue;
                }
                const bool on = event.isNoteOn();
                const std::uint32_t noteId = liveNoteId(event.device, event.channel(), event.data1);
                for (HeldLive& held : m_held) {
                    if (held.active && held.noteId == noteId && held.target == target) {
                        held.active = false; // released, or retriggered below
                    }
                }
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
                                   .velocity = on ? event.data2 : std::uint8_t{0},
                                   .part = event.channel()}};
                if (on) {
                    const auto free =
                        std::ranges::find_if(m_held, [](const HeldLive& h) { return !h.active; });
                    if (free != m_held.end()) {
                        *free = HeldLive{.target = target,
                                         .noteId = noteId,
                                         .device = event.device,
                                         .channel = event.channel(),
                                         .active = true};
                    }
                    // A note struck while the wheel is up starts bent.
                    const float cents =
                        m_bends.at(std::min<std::size_t>(event.device, kMaxMidiInputs - 1))
                            .at(event.channel())
                            .cents;
                    if (cents != 0.0F) {
                        glide(noteId, cents);
                    }
                }
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
    // midiout channels stamp their notes from this; only an engine with outputs
    // publishes it, so an offline render running beside the live engine cannot.
    if (std::ranges::any_of(core->m_midiOutputs, [](const auto& slot) {
            return slot.load(std::memory_order_relaxed) != nullptr;
        })) {
        midi::liveOutputs().blockFrame.store(time.streamFrames, std::memory_order_release);
    }
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
