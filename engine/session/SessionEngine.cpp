// adx-thread: realtime
#include "engine/session/SessionEngine.h"

#include <algorithm>
#include <cmath>

#include "engine/session/FollowAction.h"
#include "engine/session/LaunchQuantize.h"

namespace adx::session {

namespace {

constexpr std::uint32_t kNoRow = 0xFFFFFFFFU;

} // namespace

void SessionEngine::prepare() {
    if (m_runtime.view().empty()) {
        m_runtime.allocate(kMaxClips);
        m_lanes.allocate(kMaxClips);
        m_states.allocate(kMaxTracks * kMaxRows);
        m_log.allocate(kLogCapacity);
    }
}

ClipState SessionEngine::rawState(std::uint32_t track, std::uint32_t row) const noexcept {
    if (track >= kMaxTracks || row >= kMaxRows || m_states.view().empty()) {
        return ClipState::Stopped;
    }
    return static_cast<ClipState>(
        m_states.view()[(track * kMaxRows) + row].load(std::memory_order_acquire));
}

ClipState SessionEngine::state(std::uint32_t track, std::uint32_t row) const noexcept {
    return rawState(track, row);
}

float SessionEngine::progress(std::uint32_t track) const noexcept {
    return track < kMaxTracks ? m_progress.at(track).load(std::memory_order_acquire) : -1.0F;
}

void SessionEngine::setState(std::uint32_t track, std::uint32_t row, ClipState state) noexcept {
    if (track < kMaxTracks && row < kMaxRows && !m_states.view().empty()) {
        m_states.view()[(track * kMaxRows) + row].store(static_cast<std::uint8_t>(state),
                                                        std::memory_order_release);
    }
}

bool SessionEngine::takeLogged(PerformanceEvent& out) noexcept {
    const std::size_t tail = m_logTail.load(std::memory_order_relaxed);
    if (tail == m_logHead.load(std::memory_order_acquire) || m_log.view().empty()) {
        return false;
    }
    out = m_log.view()[tail % kLogCapacity];
    m_logTail.store(tail + 1, std::memory_order_release);
    return true;
}

void SessionEngine::log(const PerformanceEvent& event) noexcept {
    if (!m_recording || m_log.view().empty()) {
        return;
    }
    const std::size_t head = m_logHead.load(std::memory_order_relaxed);
    if (head - m_logTail.load(std::memory_order_acquire) >= kLogCapacity) {
        m_logLost.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    m_log.view()[head % kLogCapacity] = event;
    m_logHead.store(head + 1, std::memory_order_release);
}

SessionEngine::Runtime* SessionEngine::playing(std::uint32_t track) noexcept {
    for (Runtime& runtime : m_runtime.view()) {
        if (runtime.used && !runtime.releasing && runtime.track == track) {
            return &runtime;
        }
    }
    return nullptr;
}

void SessionEngine::request(const SessionRequest& request) noexcept {
    switch (request.kind) {
    case SessionRequest::Kind::Launch:
    case SessionRequest::Kind::Stop: {
        if (request.track >= kMaxTracks) {
            return;
        }
        Pending& pending = m_pending.at(request.track);
        // A request replaces the one before it (mashing a pad queues the last intent):
        // the row it had queued goes back to how it was.
        if (pending.waiting && !pending.stop && pending.row != request.row &&
            rawState(request.track, pending.row) == ClipState::Queued) {
            setState(request.track, pending.row, ClipState::Stopped);
        }
        const Runtime* current = playing(request.track);
        if (request.kind == SessionRequest::Kind::Stop) {
            if (current == nullptr) {
                pending = Pending{};
                return;
            }
            pending = Pending{.waiting = true, .stop = true, .row = current->row};
            setState(request.track, current->row, ClipState::QueuedStop);
            return;
        }
        pending = Pending{.waiting = true, .stop = false, .row = request.row, .gain = request.gain};
        const bool same = current != nullptr && current->row == request.row;
        setState(request.track, request.row, same ? ClipState::Retriggered : ClipState::Queued);
        return;
    }
    case SessionRequest::Kind::Scene:
        m_scene = request.row;
        m_sceneGain = request.gain;
        return;
    case SessionRequest::Kind::StopAll:
        for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
            if (const Runtime* current = playing(t)) {
                m_pending.at(t) = Pending{.waiting = true, .stop = true, .row = current->row};
                setState(t, current->row, ClipState::QueuedStop);
            } else {
                m_pending.at(t) = Pending{};
            }
        }
        return;
    case SessionRequest::Kind::Rate:
        if (request.value > 0.0) {
            m_pendingRate = std::clamp(request.value, 0.25, 4.0);
        }
        return;
    case SessionRequest::Kind::Tap: {
        const double seconds = request.value >= 0.0 ? request.value
                                                    : static_cast<double>(m_frames) /
                                                          static_cast<double>(m_sampleRate);
        const double bpm = m_tap.tap(seconds);
        if (bpm > 0.0) {
            m_tapped.store(bpm, std::memory_order_release);
            m_sceneBpm = -bpm; // taken over at the next block, not on a boundary
        }
        return;
    }
    case SessionRequest::Kind::Record:
        m_recording = request.value != 0.0;
        m_recordingPublished.store(m_recording, std::memory_order_release);
        return;
    }
}

void SessionEngine::shiftTempo(Runtime& runtime, const core::TempoView& map) noexcept {
    if (!map.valid()) {
        runtime.tempoCount = 0;
        return;
    }
    const core::Ticks origin{runtime.originTick};
    const std::size_t segment = map.segmentAt(origin);
    runtime.tempoEvents.at(0) = core::TempoEvent{
        .at = core::Ticks{0}, .bpm = map.bpmAt(origin), .ramp = map.events[segment].ramp};
    std::uint32_t count = 1;
    for (std::size_t i = segment + 1; i < map.events.size() && count < kTempoEvents; ++i) {
        const core::TempoEvent& event = map.events[i];
        if (event.at.value <= runtime.originTick) {
            continue;
        }
        runtime.tempoEvents.at(count++) =
            core::TempoEvent{.at = core::Ticks{event.at.value - runtime.originTick},
                             .bpm = event.bpm,
                             .ramp = event.ramp};
    }
    runtime.tempoCount = count;
    core::computeCumulativeSeconds(
        std::span<const core::TempoEvent>{runtime.tempoEvents.data(), count},
        std::span<double>{runtime.tempoSeconds.data(), count});
}

core::TempoView SessionEngine::tempoOf(const Runtime& runtime) noexcept {
    return core::TempoView{
        .events = std::span<const core::TempoEvent>{runtime.tempoEvents.data(), runtime.tempoCount},
        .cumSeconds = std::span<const double>{runtime.tempoSeconds.data(), runtime.tempoCount}};
}

void SessionEngine::rebind(const project::Snapshot& snapshot,
                           transport::TransportSet& transport) noexcept {
    for (Runtime& runtime : m_runtime.view()) {
        if (!runtime.used) {
            continue;
        }
        if (runtime.releasing) {
            // Its stop was delivered; it plays nothing more, and the old snapshot's
            // events are about to be retired.
            runtime.lane.track = nullptr;
            continue;
        }
        const project::SessionClip* clip = snapshot.sessionClip(runtime.track, runtime.row);
        if (clip == nullptr) {
            runtime.lane.track = nullptr;
            runtime.releasing = true;
            runtime.stopIssued = true;
            runtime.tailFrames = 0;
            setState(runtime.track, runtime.row, ClipState::Stopped);
            continue;
        }
        runtime.lane.track = &clip->events;
        runtime.lane.channel = clip->channel;
        // The map from the clip's origin again: an edited tempo is heard at once.
        shiftTempo(runtime, snapshot.tempo);
        if (runtime.tempoCount > 0) {
            transport.get(runtime.source).bindTempo(tempoOf(runtime), snapshot.sampleRate);
        }
    }
}

double SessionEngine::masterRate(const transport::TransportSet& transport) const noexcept {
    const transport::TimeSource& arrangement = transport.arrangement();
    return arrangement.rolling() ? arrangement.rate() : m_rate;
}

void SessionEngine::setRate(transport::TransportSet& transport, double rate,
                            std::int64_t tick) noexcept {
    if (rate == m_rate) {
        return;
    }
    m_rate = rate;
    m_publishedRate.store(rate, std::memory_order_release);
    transport.arrangement().requestRate(rate);
    log(PerformanceEvent{.kind = PerformanceEvent::Kind::Rate, .tick = tick, .rate = rate});
}

SessionEngine::Grid SessionEngine::grid(const project::Snapshot& snapshot,
                                        const transport::TransportSet& transport,
                                        std::uint32_t frames) const noexcept {
    Grid out;
    const transport::TimeSource& arrangement = transport.arrangement();
    if (arrangement.rolling()) {
        out.fromSamples = arrangement.positionSamples();
        out.rate = arrangement.rate();
    } else if (m_clockRunning) {
        out.fromSamples = static_cast<std::int64_t>(std::floor(m_clock));
        out.rate = m_rate;
    } else {
        return out;
    }
    const auto span = static_cast<std::int64_t>(std::ceil(static_cast<double>(frames) * out.rate));
    out.fromTicks =
        snapshot.tempo.toTicks(core::Samples{out.fromSamples}, snapshot.sampleRate).value;
    out.toTicks =
        snapshot.tempo.toTicks(core::Samples{out.fromSamples + span}, snapshot.sampleRate).value;
    out.valid = true;
    return out;
}

std::uint32_t SessionEngine::offsetOf(const project::Snapshot& snapshot, const Grid& grid,
                                      std::int64_t tick, std::uint32_t frames) noexcept {
    const std::int64_t at = snapshot.tempo.toSamples(core::Ticks{tick}, snapshot.sampleRate).value;
    const double along =
        static_cast<double>(at - grid.fromSamples) / (grid.rate > 0.0 ? grid.rate : 1.0);
    const auto offset = static_cast<std::int64_t>(std::ceil(along));
    return static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(offset, 0, static_cast<std::int64_t>(frames) - 1));
}

void SessionEngine::start(const project::Snapshot& snapshot, transport::TransportSet& transport,
                          const project::SessionClip& clip, std::uint32_t offset, std::int64_t tick,
                          std::int64_t phase, float gain, std::uint32_t passes) noexcept {
    const auto free =
        std::ranges::find_if(m_runtime.view(), [](const Runtime& r) { return !r.used; });
    if (free == m_runtime.view().end()) {
        m_refused.fetch_add(1, std::memory_order_acq_rel);
        setState(clip.track, clip.row, ClipState::Stopped);
        return;
    }
    const transport::TimeSourceId source = transport.acquire();
    if (source == transport::kInvalidTimeSource) {
        m_refused.fetch_add(1, std::memory_order_acq_rel);
        setState(clip.track, clip.row, ClipState::Stopped);
        return;
    }
    Runtime& runtime = *free;
    runtime = Runtime{};
    runtime.used = true;
    runtime.track = clip.track;
    runtime.row = clip.row;
    runtime.source = source;
    runtime.loop = clip.loop;
    runtime.passes = passes;
    runtime.length = std::max<std::int64_t>(clip.length, 1);
    phase = std::clamp<std::int64_t>(phase, 0, runtime.length - 1);
    runtime.originTick = tick - phase;
    runtime.gain = clip.gain * (gain > 0.0F ? gain : 1.0F);
    shiftTempo(runtime, snapshot.tempo);

    transport::TimeSource& time = transport.get(source);
    time.bindTempo(runtime.tempoCount > 0 ? tempoOf(runtime) : snapshot.tempo, snapshot.sampleRate);
    if (clip.loop) {
        time.requestLoop(transport::LoopRegion{
            .start = core::Ticks{0}, .end = core::Ticks{runtime.length}, .enabled = true});
    }
    // Started `offset` frames before its tick `phase`, so that tick is that frame (§4.3).
    time.requestSeekSamples(time.samplesAt(core::Ticks{phase}) - static_cast<std::int64_t>(offset));
    time.requestRate(masterRate(transport));
    time.requestState(transport::PlayState::Playing);

    runtime.lane = graph::SessionLane{.channel = clip.channel,
                                      .source = source.v,
                                      .track = &clip.events,
                                      .cursor = &runtime.cursor,
                                      .stopAt = graph::SessionLane::kNoStop,
                                      .gain = runtime.gain};
    setState(clip.track, clip.row, ClipState::Playing);
    log(PerformanceEvent{.kind = PerformanceEvent::Kind::Pass,
                         .track = clip.track,
                         .row = clip.row,
                         .tick = tick,
                         .phase = phase,
                         .length = runtime.length,
                         .gain = runtime.gain});
}

void SessionEngine::stop(std::uint32_t track, std::uint32_t offset, std::int64_t tick) noexcept {
    Runtime* runtime = playing(track);
    if (runtime == nullptr) {
        return;
    }
    runtime->lane.stopAt = offset;
    runtime->releasing = true;
    runtime->stopIssued = true;
    runtime->tailFrames = 0;
    setState(track, runtime->row, ClipState::Stopped);
    m_progress.at(std::min<std::size_t>(track, kMaxTracks - 1))
        .store(-1.0F, std::memory_order_release);
    log(PerformanceEvent{.kind = PerformanceEvent::Kind::Stop, .track = track, .tick = tick});
}

void SessionEngine::applyPending(const project::Snapshot& snapshot,
                                 transport::TransportSet& transport, std::uint32_t track,
                                 std::uint32_t offset, std::int64_t tick) noexcept {
    Pending& pending = m_pending.at(track);
    const Pending was = pending;
    pending = Pending{};
    const project::SessionClip* clip = was.stop ? nullptr : snapshot.sessionClip(track, was.row);
    if (clip == nullptr) {
        stop(track, offset, tick); // a stop, or a launch into an empty cell
        return;
    }
    const Runtime* current = playing(track);
    std::int64_t phase = 0;
    if (current != nullptr && clip->legato) {
        if (current->row == clip->row) {
            setState(track, clip->row, ClipState::Playing); // carries on in phase
            return;
        }
        // Another clip, legato: it takes over where the playing one is in its loop.
        phase = (tick - current->originTick) % std::max<std::int64_t>(clip->length, 1);
    }
    stop(track, offset, tick);
    start(snapshot, transport, *clip, offset, tick, phase, was.gain, 0);
}

void SessionEngine::passEnded(const project::Snapshot& snapshot, transport::TransportSet& transport,
                              Runtime& runtime, std::uint32_t offset, std::int64_t tick) noexcept {
    ++runtime.passes;
    const std::uint32_t track = runtime.track;
    const std::uint32_t row = runtime.row;
    const project::SessionClip* clip = snapshot.sessionClip(track, row);
    if (clip == nullptr) {
        stop(track, offset, tick);
        return;
    }
    const auto action = static_cast<FollowAction>(clip->follow);
    const std::uint32_t repeats = std::max<std::uint32_t>(clip->followRepeats, 1);
    const float gain = clip->gain > 0.0F ? runtime.gain / clip->gain : 1.0F;
    // Another pass of the same clip: a looping one wraps by itself (its source loops),
    // and only the log hears of it; a one-shot starts again on a fresh source.
    const auto again = [&] {
        if (runtime.loop) {
            runtime.originTick = tick;
            log(PerformanceEvent{.kind = PerformanceEvent::Kind::Pass,
                                 .track = track,
                                 .row = row,
                                 .tick = tick,
                                 .length = runtime.length,
                                 .gain = runtime.gain});
            return;
        }
        const std::uint32_t passes = runtime.passes;
        stop(track, offset, tick);
        start(snapshot, transport, *clip, offset, tick, 0, gain, passes);
    };
    if (action == FollowAction::None) {
        if (runtime.loop) {
            again();
        } else {
            stop(track, offset, tick); // a one-shot that ran out
        }
        return;
    }
    const FollowResult next = follow(action, row, snapshot.sessionRows, runtime.passes, repeats,
                                     snapshot.sessionSeed ^ (track * 0x9E3779B9U));
    if (action == FollowAction::RepeatN) {
        if (next.kind == FollowResult::Kind::Stop) {
            stop(track, offset, tick);
        } else {
            again();
        }
        return;
    }
    if (runtime.passes < repeats) {
        again(); // its passes before the action
        return;
    }
    if (next.kind == FollowResult::Kind::Launch) {
        const project::SessionClip* target = snapshot.sessionClip(track, next.row);
        if (target == nullptr) {
            stop(track, offset, tick); // follow into an empty cell
            return;
        }
        if (next.row == row) {
            runtime.passes = 0;
            again();
            return;
        }
        stop(track, offset, tick);
        start(snapshot, transport, *target, offset, tick, 0, gain, 0);
        return;
    }
    if (next.kind == FollowResult::Kind::Stop || !runtime.loop) {
        stop(track, offset, tick);
        return;
    }
    again();
}

void SessionEngine::resolveTrack(const project::Snapshot& snapshot,
                                 transport::TransportSet& transport, const Grid& g,
                                 std::uint32_t track, std::uint32_t frames) noexcept {
    // A request and a pass end can both fall in one block; the earlier happens first,
    // and a request on the very tick a pass ends wins over the follow action. Bounded:
    // a clip shorter than a block could end more than once in it, but not without limit.
    for (int step = 0; step < 4; ++step) {
        Pending& pending = m_pending.at(track);
        Runtime* current = playing(track);
        bool haveRequest = false;
        std::int64_t requestTick = 0;
        std::uint32_t requestOffset = 0;
        if (pending.waiting) {
            const project::SessionClip* clip = snapshot.sessionClip(track, pending.row);
            const project::SessionClip* playingClip =
                current != nullptr ? snapshot.sessionClip(track, current->row) : nullptr;
            const project::SessionClip* quantizedBy =
                pending.stop || clip == nullptr ? playingClip : clip;
            if (quantizedBy == nullptr) {
                // Nothing to launch and nothing playing to stop.
                if (pending.row < kMaxRows && rawState(track, pending.row) == ClipState::Queued) {
                    setState(track, pending.row, ClipState::Stopped);
                }
                pending = Pending{};
            } else {
                const auto quantize = static_cast<LaunchQuantize>(quantizedBy->quantize);
                if (quantize == LaunchQuantize::Instant) {
                    haveRequest = true;
                    requestTick = g.fromTicks;
                    requestOffset = 0;
                } else {
                    const std::int64_t boundary =
                        nextBoundary(g.fromTicks, quantize, snapshot.sessionBarTicks);
                    if (boundary < g.toTicks) {
                        haveRequest = true;
                        requestTick = boundary;
                        requestOffset = offsetOf(snapshot, g, boundary, frames);
                    }
                }
            }
        }
        bool havePass = false;
        std::int64_t passTick = 0;
        if (current != nullptr) {
            passTick = current->originTick + current->length;
            havePass = passTick < g.toTicks;
        }
        if (!haveRequest && !havePass) {
            return;
        }
        if (haveRequest && (!havePass || requestTick <= passTick)) {
            applyPending(snapshot, transport, track, requestOffset, requestTick);
            continue;
        }
        passEnded(snapshot, transport, *current, offsetOf(snapshot, g, passTick, frames), passTick);
    }
}

std::span<const graph::SessionLane> SessionEngine::beforeRender(const project::Snapshot& snapshot,
                                                                transport::TransportSet& transport,
                                                                std::uint32_t frames) noexcept {
    m_sampleRate = std::max<std::uint32_t>(snapshot.sampleRate, 1);
    // A scene: every track's cell in that row launches; an empty one stops its track
    // where the track says so (§4.1); its tempo takes over on the global grid.
    if (m_scene != kNoRow) {
        const std::uint32_t row = m_scene;
        m_scene = kNoRow;
        for (std::uint32_t t = 0; t < snapshot.sessionTracks && t < kMaxTracks; ++t) {
            if (snapshot.sessionClip(t, row) != nullptr) {
                request(SessionRequest{.kind = SessionRequest::Kind::Launch,
                                       .track = t,
                                       .row = row,
                                       .gain = m_sceneGain});
            } else if (t < snapshot.trackStopOnEmpty.size() && snapshot.trackStopOnEmpty[t] != 0) {
                request(SessionRequest{.kind = SessionRequest::Kind::Stop, .track = t, .row = 0});
            }
        }
        if (row < snapshot.sceneTempo.size() && snapshot.sceneTempo[row] > 0.0) {
            m_sceneBpm = snapshot.sceneTempo[row];
        }
    }
    const bool anyPending =
        std::ranges::any_of(m_pending, [](const Pending& p) { return p.waiting; });
    if (!transport.arrangement().rolling() && !m_clockRunning && (anyPending || m_recording)) {
        // Nothing playing: the session's own grid begins now, so the launch is now.
        m_clockRunning = true;
        m_clock = 0.0;
    }
    Grid g = grid(snapshot, transport, frames);
    bool rateSet = false;
    // Rates first, so this block's grid and every clip run at the new one. A tap (a
    // negative bpm) and a nudge take over at once; a scene's tempo at its boundary.
    // A nudge or a tap applies whether or not anything plays: with nothing playing it
    // is the rate the next launch starts at.
    const std::int64_t here = g.valid ? g.fromTicks : 0;
    if (m_pendingRate > 0.0) {
        setRate(transport, m_pendingRate, here);
        m_pendingRate = 0.0;
        rateSet = true;
    } else if (m_sceneBpm < 0.0) {
        setRate(transport, -m_sceneBpm / snapshot.tempo.bpmAt(core::Ticks{here}), here);
        m_sceneBpm = 0.0;
        rateSet = true;
    } else if (g.valid) {
        if (m_sceneBpm > 0.0) {
            const std::int64_t boundary =
                nextBoundary(g.fromTicks, static_cast<LaunchQuantize>(snapshot.sessionQuantize),
                             snapshot.sessionBarTicks);
            if (boundary < g.toTicks) {
                // From the block after the boundary: the block it falls in plays out at
                // the old rate, so nothing before the boundary moves.
                m_pendingRate =
                    std::clamp(m_sceneBpm / snapshot.tempo.bpmAt(core::Ticks{boundary}), 0.25, 4.0);
                m_sceneBpm = 0.0;
            }
        }
    }
    // The arrangement takes a requested rate at its next block start, which is this
    // block's: what was just set here is the rate this block runs at.
    const double rate = rateSet ? m_rate : masterRate(transport);
    if (rateSet && g.valid) {
        g.rate = rate;
        const auto span = static_cast<std::int64_t>(std::ceil(static_cast<double>(frames) * rate));
        g.toTicks =
            snapshot.tempo.toTicks(core::Samples{g.fromSamples + span}, snapshot.sampleRate).value;
    }
    if (g.valid) {
        for (std::uint32_t t = 0; t < kMaxTracks; ++t) {
            resolveTrack(snapshot, transport, g, t, frames);
        }
    }
    // Every playing clip runs at the master's rate this block.
    std::size_t count = 0;
    for (Runtime const& runtime : m_runtime.view()) {
        if (!runtime.used) {
            continue;
        }
        if (!runtime.releasing && transport.get(runtime.source).rate() != rate) {
            transport.get(runtime.source).requestRate(rate);
        }
        if (runtime.lane.track != nullptr) {
            m_lanes.view()[count++] = runtime.lane;
        }
    }
    return m_lanes.view().first(count);
}

void SessionEngine::afterRender(const project::Snapshot& /*snapshot*/,
                                transport::TransportSet& transport, std::uint32_t frames,
                                std::span<std::atomic<std::uint8_t>> sounding) noexcept {
    m_frames += frames;
    std::uint32_t active = 0;
    for (Runtime& runtime : m_runtime.view()) {
        if (!runtime.used) {
            continue;
        }
        ++active;
        transport::TimeSource& time = transport.get(runtime.source);
        const bool voices = runtime.source.v < sounding.size() &&
                            sounding[runtime.source.v].exchange(0, std::memory_order_acq_rel) != 0;
        if (runtime.releasing) {
            if (runtime.stopIssued) {
                // The stop's offset reached the lane this block; the source stops now and
                // keeps its id while the clip's voices release.
                runtime.stopIssued = false;
                runtime.lane.stopAt = graph::SessionLane::kNoStop;
                runtime.lane.track = nullptr;
                time.requestState(transport::PlayState::Stopped);
                continue;
            }
            runtime.tailFrames += frames;
            // Back to the pool once a whole block went by with none of its voices heard.
            if (!voices || runtime.tailFrames >= kMaxTailFrames) {
                transport.release(runtime.source);
                runtime = Runtime{};
            }
            continue;
        }
        const double local = static_cast<double>(time.positionTicks().value);
        m_progress.at(std::min<std::size_t>(runtime.track, kMaxTracks - 1))
            .store(std::clamp(static_cast<float>(local / static_cast<double>(runtime.length)), 0.0F,
                              1.0F),
                   std::memory_order_release);
    }
    m_active.store(active, std::memory_order_release);
    if (!transport.arrangement().rolling() && m_clockRunning) {
        m_clock += static_cast<double>(frames) * m_rate;
        const bool anyPending =
            std::ranges::any_of(m_pending, [](const Pending& p) { return p.waiting; });
        if (active == 0 && !anyPending && !m_recording) {
            m_clockRunning = false;
        }
    }
}

} // namespace adx::session
