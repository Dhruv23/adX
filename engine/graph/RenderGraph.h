// A graph, compiled down to what the audio thread executes: a flat list of steps in
// topological order, the edges each step feeds, and the buffers they run on.
//
// Everything topological - cycle detection, the order, plugin delay compensation,
// which buffer each port lands in - was decided on the main thread when this was
// built. The audio thread walks the arrays front to back and never asks a question
// about the graph's shape (phase_3.md §4.4, §4.6).
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/graph/BufferPool.h"
#include "engine/graph/DelayLine.h"
#include "engine/graph/Node.h"
#include "engine/graph/NodeId.h"
#include "engine/graph/PortSpec.h"

namespace adx::graph {

/// One node's turn.
struct NodeStep {
    Node* node{nullptr};
    NodeId id;
    /// Accumulators the scheduler sums this node's inputs into, per input port.
    /// kNone for a port nothing is routed to - the node then reads silence.
    std::array<std::uint32_t, kMaxInputPorts> inputBuffers{kNone, kNone};
    /// Where the node writes, per output port. kNone for a port it does not have.
    std::array<std::uint32_t, kMaxOutputPorts> outputBuffers{kNone, kNone};
    /// Outgoing edges, applied right after the node runs: [firstEdge, +edgeCount).
    std::uint32_t firstEdge{0};
    std::uint32_t edgeCount{0};
    /// Accumulators that start their life at this step and must be zeroed before
    /// it: [firstClear, +clearCount) into RenderGraph::clears.
    std::uint32_t firstClear{0};
    std::uint32_t clearCount{0};
    /// Index into the snapshot's event tracks, or kNone for a node that takes no
    /// events.
    std::uint32_t eventTrack{kNone};
    /// Which time source drives this node. 0, the arrangement, until Phase 11.
    std::uint32_t timeSource{0};
    /// This node's slice of the snapshot's parameter storage.
    std::uint32_t paramBase{0};
    std::uint32_t paramCount{0};
};

/// Sums one output port of the step that owns it into another step's accumulator.
struct EdgeStep {
    std::uint32_t fromPort{kPortPost};
    std::uint32_t toBuffer{kNone};
    /// PDC: a delay line that holds this edge back so it arrives aligned with the
    /// slowest path into the same accumulator. kNone when no delay is needed - which,
    /// with every latency at zero, is every edge (pdc_zero_latency_inserts_nothing).
    std::uint32_t delayLine{kNone};
};

struct RenderGraph {
    std::span<const NodeStep> steps;
    std::span<const EdgeStep> edges;
    std::span<const std::uint32_t> clears;
    BufferPool buffers;
    std::span<DelayLine> delays;
    /// The master's post-fader buffer: what goes to the device. kNone for a project
    /// with no master, which renders silence.
    std::uint32_t outputBuffer{kNone};
    /// Samples between an event and its sound at the output, along the slowest path.
    /// The latency readout, and Phase 8's recording alignment.
    std::uint32_t totalLatency{0};
};

} // namespace adx::graph
