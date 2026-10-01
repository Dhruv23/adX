#include "engine/graph/Scheduler.h"

#include <algorithm>

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
        block.kind = event.kind == project::EventKind::NoteOn ? BlockEventKind::NoteOn
                                                              : BlockEventKind::NoteOff;
        block.pitch = event.pitch;
        block.velocity = event.velocity;
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
    if (snapshot.automation.empty() || !arrangement.rolling()) {
        return;
    }
    // Control rate: once per piece, at its first sample. Sample-accurate parameter
    // smoothing is DSP and belongs to the Phase 4 nodes that consume the values.
    const std::int64_t tick = arrangement.positionTicks().value;
    for (const project::AutomationLane& lane : snapshot.automation) {
        if (tick >= lane.startTick && tick < lane.endTick &&
            lane.paramIndex < snapshot.params.size()) {
            snapshot.params[lane.paramIndex] = lane.valueAt(tick);
        }
    }
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
