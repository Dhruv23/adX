// The audio node interface.
//
// Implementing an instrument or an effect is one class: prepare, process,
// latencySamples, reset (phase_3.md §9). Everything a node needs during process()
// arrives in the ProcessContext - its buffers, its events already filtered to this
// node and this block, its parameters already resolved to floats - so a node never
// parses, never looks anything up by string, never allocates, and never reads a
// clock it was not handed.
#pragma once

#include <cstdint>
#include <span>

#include "engine/graph/PortSpec.h"
#include "engine/rt/BlockArena.h"
#include "engine/transport/TimeSource.h"

namespace adx::graph {

struct PrepareInfo {
    std::uint32_t sampleRate{48000};
    /// The largest `frames` process() will ever see. Size history buffers from this.
    std::uint32_t maxBlockFrames{2048};
};

/// What an instrument is told to do, at a sample offset within this call.
enum class BlockEventKind : std::uint8_t {
    NoteOff,
    NoteOn,
    /// Release every voice driven by `timeSource` - a stop, or a seek while playing.
    /// A release, never a cut (phase_3.md §4.11).
    ReleaseAll,
    /// The source wrapped its loop. Release the voices whose note-off lies at or
    /// beyond `endTick`, the loop end: they would otherwise never see it.
    LoopWrap,
};

/// One event, positioned in the block. Built on the audio thread from the snapshot's
/// ScheduledEvents; the conversion from ticks to an offset has already happened.
///
/// No member initialisers: this comes from the per-callback arena, which hands out
/// only trivially constructible types. Every site that builds one value-initialises it.
struct BlockEvent {
    /// Frames from the start of this process() call.
    std::uint32_t offset;
    std::uint32_t noteId;
    std::uint32_t instance;
    std::uint32_t timeSource;
    /// NoteOn: the matching note-off's tick. LoopWrap: the loop end's tick.
    std::int64_t endTick;
    BlockEventKind kind;
    std::uint8_t pitch;
    std::uint8_t velocity;
};

/// Sorted by offset. Dispatch is one advancing index over this, never a rescan.
using EventView = std::span<const BlockEvent>;

struct ProcessContext {
    /// THE CHECKPOINT (phase_3.md §4.3). The source driving this node, passed in.
    /// There is no global position to read instead, because there is not one.
    const transport::TimeSource& time;
    /// Planar, port-major: outputs[port * kPortChannels + channel]. Each is `frames`
    /// long. A node overwrites every output sample; nothing is pre-cleared for it.
    std::span<const std::span<float>> outputs;
    /// Same layout. An input port nothing is routed to is silence, not absent.
    std::span<const std::span<const float>> inputs;
    std::uint32_t frames{0};
    std::uint32_t sampleRate{48000};
    EventView events;
    /// This node's own parameters, in the order its kind defines. A slice of the
    /// snapshot's parameter storage, so a knob turn that arrived this block is already
    /// in it.
    std::span<const float> params;
    /// Per-callback scratch. Reset before the first node runs; anything taken from it
    /// is gone next block.
    rt::BlockArena& arena;
};

class Node {
public:
    Node() = default;
    virtual ~Node() = default;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    Node(Node&&) = delete;
    Node& operator=(Node&&) = delete;

    /// Main thread. Called once, when the node first enters a graph. Allocate here.
    virtual void prepare(const PrepareInfo& info) = 0;

    /// AUDIO THREAD. No allocation, no locks, no exceptions, no unbounded work.
    virtual void process(ProcessContext& context) noexcept = 0;

    /// Main thread. Samples of latency this node introduces. PDC reads it when the
    /// graph is built; the audio thread never asks.
    [[nodiscard]] virtual std::uint32_t latencySamples() const noexcept {
        return 0;
    }

    /// Clears DSP state. Called only for nodes whose output depends on the timeline
    /// position; a reverb tail survives a seek (phase_3.md §4.11).
    virtual void reset() noexcept = 0;

    [[nodiscard]] virtual PortSpec ports() const noexcept = 0;
};

} // namespace adx::graph
