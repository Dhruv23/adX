#include "engine/graph/Scheduler.h"

#include <algorithm>
#include <limits>

#include "engine/core/Config.h"
#include "engine/rt/RtConfig.h"
#include "engine/transport/Seek.h"

namespace adx::graph {
namespace {

/// What a node reads from an input port nothing is routed to.
constexpr std::array<float, rt::kMaxBlockFrames> kSilence{};

using PortInputs =
    std::array<std::span<const float>, static_cast<std::size_t>(kMaxInputPorts) * kPortChannels>;
using PortOutputs =
    std::array<std::span<float>, static_cast<std::size_t>(kMaxOutputPorts) * kPortChannels>;

/// The source-timeline sample that output frame `frame` of this piece plays.
[[nodiscard]] std::int64_t sampleOfFrame(const transport::TimeSource& time, std::int64_t start,
                                         std::uint32_t frame) noexcept {
    if (time.rate() == 1.0) {
        return start + frame;
    }
    return start + static_cast<std::int64_t>(static_cast<double>(frame) * time.rate());
}

/// Writes one lane's values over the frames of [first, first + out.size()) it covers.
///
/// A pure function of the sample position: the knot whose segment contains a sample
/// is found by its own sample position, and the value is interpolated from the two
/// knots' positions in double precision. Nothing carries over from the previous
/// block, so the values are the same whatever the block size (phase_4.md §4.0) - and
/// a seek into the middle of a ramp lands on the ramp's value at that sample, not on
/// a value replayed from the ramp's start.
void evaluateLane(const project::AutomationLane& lane, const transport::TimeSource& time,
                  std::int64_t start, std::span<float> out) noexcept {
    const std::span<const project::Knot> knots = lane.knots;
    if (knots.empty()) {
        return;
    }
    const std::int64_t laneStart = time.samplesAt(core::Ticks{lane.startTick});
    const std::int64_t laneEnd = lane.endTick == std::numeric_limits<std::int64_t>::max()
                                     ? std::numeric_limits<std::int64_t>::max()
                                     : time.samplesAt(core::Ticks{lane.endTick});
    const auto frames = static_cast<std::uint32_t>(out.size());

    // The first knot after the piece's first covered sample, found by binary search -
    // then walked forward frame by frame.
    std::uint32_t frame = 0;
    while (frame < frames && sampleOfFrame(time, start, frame) < laneStart) {
        ++frame;
    }
    if (frame == frames) {
        return;
    }
    const std::int64_t firstSample = sampleOfFrame(time, start, frame);
    std::size_t low = 0;
    std::size_t high = knots.size();
    while (low < high) {
        const std::size_t mid = low + ((high - low) / 2);
        if (time.samplesAt(core::Ticks{knots[mid].tick}) <= firstSample) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    std::size_t next = low; // first knot strictly after firstSample
    std::int64_t nextSample = next < knots.size() ? time.samplesAt(core::Ticks{knots[next].tick})
                                                  : std::numeric_limits<std::int64_t>::max();
    std::int64_t fromSample = next > 0 ? time.samplesAt(core::Ticks{knots[next - 1].tick}) : 0;

    for (; frame < frames; ++frame) {
        const std::int64_t sample = sampleOfFrame(time, start, frame);
        if (sample >= laneEnd) {
            break;
        }
        while (sample >= nextSample) {
            fromSample = nextSample;
            ++next;
            nextSample = next < knots.size() ? time.samplesAt(core::Ticks{knots[next].tick})
                                             : std::numeric_limits<std::int64_t>::max();
        }
        if (next == 0) {
            out[frame] = knots.front().value;
        } else if (next >= knots.size()) {
            out[frame] = knots.back().value;
        } else {
            const project::Knot& from = knots[next - 1];
            const project::Knot& to = knots[next];
            const double fraction = static_cast<double>(sample - fromSample) /
                                    static_cast<double>(nextSample - fromSample);
            out[frame] =
                static_cast<float>(static_cast<double>(from.value) +
                                   (static_cast<double>(to.value - from.value) * fraction));
        }
    }
}

/// Parameter `param`'s values over a piece: its base, overwritten by each of its lanes
/// in priority order where that lane applies.
void evaluateParam(const project::Snapshot& snapshot, std::uint32_t param,
                   const transport::TimeSource& time, std::int64_t start,
                   std::span<float> out) noexcept {
    std::ranges::fill(out, snapshot.paramBase[param]);
    const std::uint32_t first = snapshot.paramLaneStart[param];
    const std::uint32_t last = snapshot.paramLaneStart[param + 1];
    for (std::uint32_t l = first; l < last; ++l) {
        evaluateLane(snapshot.automation[snapshot.paramLanes[l]], time, start, out);
    }
}

[[nodiscard]] BlockEventKind blockKindOf(project::EventKind kind) noexcept {
    switch (kind) {
    case project::EventKind::NoteOff:
        return BlockEventKind::NoteOff;
    case project::EventKind::PitchGlide:
        return BlockEventKind::PitchGlide;
    case project::EventKind::Lyric:
        return BlockEventKind::Lyric;
    case project::EventKind::NoteOn:
    case project::EventKind::Param:
        break;
    }
    return BlockEventKind::NoteOn;
}

} // namespace

void Scheduler::reset() noexcept {
    m_releasePending.fill(false);
    m_wrapPending.fill(false);
}

std::uint32_t Scheduler::lowerBound(const transport::TimeSource& time,
                                    const project::EventTrack& track,
                                    std::int64_t samples) noexcept {
    ++m_stats.cursorSearches;
    std::size_t low = 0;
    std::size_t high = track.events.size();
    while (low < high) {
        const std::size_t mid = low + ((high - low) / 2);
        ++m_stats.searchProbes;
        if (time.samplesAt(core::Ticks{track.events[mid].tick}) < samples) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return static_cast<std::uint32_t>(low);
}

EventRange Scheduler::advanceCursor(const transport::TimeSource& time,
                                    const project::EventTrack& track, project::EventCursor& cursor,
                                    std::uint32_t frames, bool wrapped) noexcept {
    // Only on seek, tempo rebind or a fresh snapshot. This is the whole difference
    // from iteration one: the search happens when the position *jumps*, not every
    // block (phase_3.md §4.5 step 1).
    if (cursor.seekGeneration != time.seekGeneration()) {
        cursor.index = lowerBound(time, track, time.positionSamples());
        cursor.seekGeneration = time.seekGeneration();
    }
    // The loop-start position is found once, when the loop's sample bounds change,
    // so that a wrap is an assignment (scheduler_no_rescan_on_loop_wrap).
    if (time.loop().active() && cursor.loopGeneration != time.loopGeneration()) {
        cursor.loopIndex = lowerBound(time, track, time.loopStartSamples());
        cursor.loopGeneration = time.loopGeneration();
    }
    if (wrapped && time.loop().active()) {
        cursor.index = cursor.loopIndex;
    }

    if (!time.rolling()) {
        return EventRange{.begin = cursor.index, .end = cursor.index};
    }

    // The window this piece covers on the source's own timeline. At rate 1 it is
    // exactly `frames`; otherwise the source moves frames * rate samples.
    const std::int64_t start = time.positionSamples();
    std::int64_t span = frames;
    if (time.rate() != 1.0) {
        span = static_cast<std::int64_t>(static_cast<double>(frames) * time.rate()) + 1;
    }
    const std::int64_t windowEnd = start + span;

    // Walk forward while events start before the window ends (§4.5 step 2), then stop;
    // the cursor persists to the next block (step 3).
    const std::uint32_t begin = cursor.index;
    std::uint32_t index = begin;
    const auto size = static_cast<std::uint32_t>(track.events.size());
    while (index < size) {
        ++m_stats.eventComparisons;
        if (time.samplesAt(core::Ticks{track.events[index].tick}) >= windowEnd) {
            break;
        }
        ++index;
    }
    cursor.index = index;
    return EventRange{.begin = begin, .end = index};
}

std::uint32_t Scheduler::fillEvents(const transport::TimeSource& time, std::uint32_t timeSource,
                                    const project::EventTrack& track, EventRange range,
                                    std::span<BlockEvent> out, std::uint32_t first) noexcept {
    const std::int64_t start = time.positionSamples();
    const double rate = time.rate();
    std::uint32_t written = first;
    for (std::uint32_t i = range.begin; i < range.end && written < out.size(); ++i) {
        const project::ScheduledEvent& event = track.events[i];
        const std::int64_t at = time.samplesAt(core::Ticks{event.tick});
        if (at < start) {
            // Cannot happen with a correctly positioned cursor; if it ever does, a late
            // event is dropped rather than played at a negative offset.
            continue;
        }
        std::int64_t offset = at - start;
        if (rate != 1.0) {
            offset = static_cast<std::int64_t>(static_cast<double>(offset) / rate);
        }
        BlockEvent& block = out[written];
        block.offset = static_cast<std::uint32_t>(offset);
        block.noteId = event.noteId;
        block.instance = event.instance;
        block.timeSource = timeSource;
        block.endTick = event.endTick;
        block.kind = blockKindOf(event.kind);
        block.pitch = event.pitch;
        block.velocity = event.velocity;
        block.value = event.value;
        block.duration = 0;
        if (event.kind == project::EventKind::PitchGlide && event.endTick > event.tick) {
            // The glide's length on this source's timeline, in output frames. Converted
            // here, per source, because two sources at two tempos play one tick span in
            // two different numbers of samples.
            auto length = static_cast<double>(time.samplesAt(core::Ticks{event.endTick}) - at);
            if (rate != 1.0) {
                length /= rate;
            }
            block.duration = static_cast<std::uint32_t>(std::max(0.0, length));
        }
        ++written;
    }
    m_stats.eventsDispatched += written - first;
    return written;
}

EventView Scheduler::eventsFor(const NodeStep& step, project::Snapshot& snapshot,
                               transport::TransportSet& transport, rt::BlockArena& arena,
                               std::uint32_t frames) noexcept {
    const std::uint32_t source = step.timeSource;
    const transport::TimeSource& time = transport.get(transport::TimeSourceId{source});
    const bool release = source < m_releasePending.size() && m_releasePending[source];
    const bool wrapped = source < m_wrapPending.size() && m_wrapPending[source];

    EventRange range{};
    if (step.eventTrack < snapshot.eventTracks.size() &&
        step.eventTrack < snapshot.cursors.size()) {
        range = advanceCursor(time, snapshot.eventTracks[step.eventTrack],
                              snapshot.cursors[step.eventTrack], frames, wrapped);
    }

    const std::uint32_t special = (release ? 1U : 0U) + (wrapped ? 1U : 0U);
    const std::uint32_t count = (range.end - range.begin) + special;
    if (count == 0) {
        return {};
    }
    // From the per-callback arena, which is what the archived engine's per-callback
    // std::vector<ScheduledEvent> becomes (FINAL_PLAN §3.3.1). An overflow records a
    // violation and returns nothing; the block then plays without its events rather
    // than writing past the arena.
    const std::span<BlockEvent> events = arena.allocate<BlockEvent>(count);
    if (events.size() != count) {
        return {};
    }

    std::uint32_t written = 0;
    if (release) {
        events[written++] = BlockEvent{.offset = 0,
                                       .noteId = 0,
                                       .instance = 0,
                                       .timeSource = source,
                                       .endTick = 0,
                                       .value = 0.0F,
                                       .duration = 0,
                                       .kind = BlockEventKind::ReleaseAll,
                                       .pitch = 0,
                                       .velocity = 0};
    }
    if (wrapped) {
        events[written++] = BlockEvent{.offset = 0,
                                       .noteId = 0,
                                       .instance = 0,
                                       .timeSource = source,
                                       .endTick = time.loop().end.value,
                                       .value = 0.0F,
                                       .duration = 0,
                                       .kind = BlockEventKind::LoopWrap,
                                       .pitch = 0,
                                       .velocity = 0};
    }
    if (step.eventTrack < snapshot.eventTracks.size()) {
        written =
            fillEvents(time, source, snapshot.eventTracks[step.eventTrack], range, events, written);
    }
    return EventView{events.data(), written};
}

void Scheduler::applyAutomation(project::Snapshot& snapshot,
                                const transport::TimeSource& arrangement) noexcept {
    if (snapshot.automation.empty() || !arrangement.rolling() ||
        snapshot.paramLaneStart.size() != snapshot.params.size() + 1) {
        return;
    }
    // Every automated parameter's block value is its value at the piece's first frame,
    // for the nodes that read `params` rather than the per-frame view. Each parameter
    // is evaluated once however many lanes drive it.
    const std::int64_t start = arrangement.positionSamples();
    std::uint32_t previous = graph::kNone;
    for (std::uint32_t l = 0; l < snapshot.paramLanes.size(); ++l) {
        const std::uint32_t param = snapshot.automation[snapshot.paramLanes[l]].paramIndex;
        if (param == previous || param >= snapshot.params.size()) {
            continue;
        }
        previous = param;
        float value = 0.0F;
        evaluateParam(snapshot, param, arrangement, start, std::span<float>{&value, 1});
        snapshot.params[param] = value;
    }
}

std::span<const float* const> Scheduler::automationFor(const NodeStep& step,
                                                       project::Snapshot& snapshot,
                                                       const transport::TimeSource& arrangement,
                                                       rt::BlockArena& arena,
                                                       std::uint32_t frames) noexcept {
    if (!arrangement.rolling() || !snapshot.anyAutomated(step.paramBase, step.paramCount)) {
        return {};
    }
    const std::span<const float*> tracks = arena.allocate<const float*>(step.paramCount);
    if (tracks.size() != step.paramCount) {
        return {};
    }
    const std::int64_t start = arrangement.positionSamples();
    for (std::uint32_t i = 0; i < step.paramCount; ++i) {
        const std::uint32_t param = step.paramBase + i;
        tracks[i] = nullptr;
        if (snapshot.paramLaneStart[param + 1] == snapshot.paramLaneStart[param]) {
            continue;
        }
        const std::span<float> values = arena.allocate<float>(frames);
        if (values.size() == frames) {
            evaluateParam(snapshot, param, arrangement, start, values);
            tracks[i] = values.data();
        }
    }
    return tracks;
}

void Scheduler::runPiece(project::Snapshot& snapshot, transport::TransportSet& transport,
                         rt::BlockArena& arena, float* out, std::uint32_t offset,
                         std::uint32_t frames, std::uint32_t outChannels) noexcept {
    ++m_stats.subBlocks;
    applyAutomation(snapshot, transport.arrangement());

    const RenderGraph& graph = snapshot.graph;
    const BufferPool& buffers = graph.buffers;
    const std::span<const float> silence{kSilence.data(), frames};

    for (const NodeStep& step : graph.steps) {
        // Everything this step takes from the arena - its events, its instrument's
        // scratch - is dead once its edges have been applied.
        const std::size_t arenaMark = arena.mark();
        for (std::uint32_t c = 0; c < step.clearCount; ++c) {
            const std::uint32_t buffer = graph.clears[step.firstClear + c];
            for (std::uint32_t channel = 0; channel < kPortChannels; ++channel) {
                std::ranges::fill(buffers.channel(buffer, channel, offset, frames), 0.0F);
            }
        }

        const PortSpec ports = step.node->ports();
        PortInputs inputs{};
        PortOutputs outputs{};
        for (std::uint32_t port = 0; port < ports.inputs && port < kMaxInputPorts; ++port) {
            const std::uint32_t buffer = step.inputBuffers[port];
            for (std::uint32_t channel = 0; channel < kPortChannels; ++channel) {
                inputs[(port * kPortChannels) + channel] =
                    buffer == kNone ? silence : buffers.channel(buffer, channel, offset, frames);
            }
        }
        for (std::uint32_t port = 0; port < ports.outputs && port < kMaxOutputPorts; ++port) {
            const std::uint32_t buffer = step.outputBuffers[port];
            ADX_ASSERT(buffer != kNone);
            for (std::uint32_t channel = 0; channel < kPortChannels; ++channel) {
                outputs[(port * kPortChannels) + channel] =
                    buffers.channel(buffer, channel, offset, frames);
            }
        }

        const EventView events = step.eventTrack != kNone || ports.acceptsEvents
                                     ? eventsFor(step, snapshot, transport, arena, frames)
                                     : EventView{};

        const std::span<const float* const> automation =
            automationFor(step, snapshot, transport.arrangement(), arena, frames);

        ProcessContext context{
            .time = transport.get(transport::TimeSourceId{step.timeSource}),
            .outputs =
                std::span<const std::span<float>>{
                    outputs.data(), static_cast<std::size_t>(ports.outputs) * kPortChannels},
            .inputs =
                std::span<const std::span<const float>>{
                    inputs.data(), static_cast<std::size_t>(ports.inputs) * kPortChannels},
            .frames = frames,
            .sampleRate = snapshot.sampleRate,
            .events = events,
            .params = snapshot.params.subspan(step.paramBase, step.paramCount),
            .automation = automation,
            .arena = arena,
        };
        step.node->process(context);

        // Push this node's output into every consumer's accumulator, now, in a fixed
        // order. Summation order is part of the output's bits, which is why it is
        // decided by the builder and not by whichever consumer happens to run first
        // (phase_3.md §4.4, deterministic tie-breaking).
        for (std::uint32_t e = 0; e < step.edgeCount; ++e) {
            const EdgeStep& edge = graph.edges[step.firstEdge + e];
            const std::uint32_t from = step.outputBuffers[edge.fromPort];
            const std::span<float> srcLeft = buffers.channel(from, 0, offset, frames);
            const std::span<float> srcRight = buffers.channel(from, 1, offset, frames);
            const std::span<float> dstLeft = buffers.channel(edge.toBuffer, 0, offset, frames);
            const std::span<float> dstRight = buffers.channel(edge.toBuffer, 1, offset, frames);
            if (edge.delayLine != kNone && edge.delayLine < graph.delays.size()) {
                graph.delays[edge.delayLine].processAdd(srcLeft, srcRight, dstLeft, dstRight);
            } else {
                for (std::uint32_t i = 0; i < frames; ++i) {
                    dstLeft[i] += srcLeft[i];
                    dstRight[i] += srcRight[i];
                }
            }
        }
        arena.rewind(arenaMark);
    }

    if (graph.outputBuffer == kNone || out == nullptr || outChannels == 0) {
        return;
    }
    const std::span<float> left = buffers.channel(graph.outputBuffer, 0, offset, frames);
    const std::span<float> right = buffers.channel(graph.outputBuffer, 1, offset, frames);
    float* frame = out + (static_cast<std::size_t>(offset) * static_cast<std::size_t>(outChannels));
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (outChannels == 1) {
            frame[0] = (left[i] + right[i]) * 0.5F;
        } else {
            frame[0] = left[i];
            frame[1] = right[i];
        }
        frame += outChannels;
    }
}

void Scheduler::render(project::Snapshot& snapshot, transport::TransportSet& transport,
                       rt::BlockArena& arena, float* out, std::uint32_t frames,
                       std::uint32_t outChannels) noexcept {
    ++m_stats.blocks;

    // Every in-use source applies its requests at the same block boundary.
    for (std::uint32_t s = 0; s < transport::kMaxTimeSources; ++s) {
        const transport::TimeSourceId id{s};
        if (!transport.inUse(id)) {
            continue;
        }
        const transport::BlockTransition transition = transport.get(id).beginBlock();
        if (transport::effectsOf(transition).releaseVoices) {
            m_releasePending[s] = true;
        }
        if (transition.seeked) {
            // A seek supersedes a wrap that had not been acted on yet: the cursor is
            // about to be searched from the new position anyway.
            m_wrapPending[s] = false;
        }
    }

    std::uint32_t done = 0;
    while (done < frames) {
        // The piece ends at the nearest boundary of any source. Each source's boundary
        // is a pure function of its position and tempo map, which is exactly what makes
        // the split points - and so the output - identical offline and realtime
        // (phase_3.md §4.5, sub-block splitting).
        std::uint32_t piece = frames - done;
        for (std::uint32_t s = 0; s < transport::kMaxTimeSources; ++s) {
            const transport::TimeSourceId id{s};
            if (transport.inUse(id)) {
                piece = std::min(piece, transport.get(id).framesToNextBoundary(piece));
            }
        }
        if (piece == 0) {
            piece = frames - done;
        }

        runPiece(snapshot, transport, arena, out, done, piece, outChannels);

        for (std::uint32_t s = 0; s < transport::kMaxTimeSources; ++s) {
            const transport::TimeSourceId id{s};
            if (!transport.inUse(id)) {
                continue;
            }
            // Pending flags were delivered to every step in the piece that just ran.
            m_releasePending[s] = false;
            m_wrapPending[s] = transport.get(id).advance(piece);
        }
        done += piece;
    }

    for (std::uint32_t s = 0; s < transport::kMaxTimeSources; ++s) {
        const transport::TimeSourceId id{s};
        if (transport.inUse(id)) {
            transport.get(id).publish();
        }
    }
}

} // namespace adx::graph
