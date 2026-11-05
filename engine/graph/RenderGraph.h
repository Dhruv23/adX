// A graph, compiled down to what the audio thread executes: a flat list of steps in
// topological order, the edges each step feeds, and the buffers they run on.
//
// Everything topological - cycle detection, the order, plugin delay compensation,
// which buffer each port lands in - was decided on the main thread when this was
// built. The audio thread walks the arrays front to back and never asks a question
// about the graph's shape (phase_3.md §4.4, §4.6).
#pragma once

#include <array>
#include <atomic>
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
    /// Index into the snapshot's audio tracks, or kNone (AudioRegion.h).
    std::uint32_t audioTrack{kNone};
    /// Which time source drives this node. 0, the arrangement, until Phase 11.
    std::uint32_t timeSource{0};
    /// This node's slice of the snapshot's parameter storage.
    std::uint32_t paramBase{0};
    std::uint32_t paramCount{0};
    /// A controller's target parameter, written from Node::modulation() after the node
    /// runs; kNone for every other node.
    std::uint32_t modulates{kNone};
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

/// One input a step gathers in the parallel plan: a producer's output port, added into
/// the step's accumulator - through its PDC delay line when it has one.
struct GatherStep {
    /// The producer's output, in ParallelPlan::buffers.
    std::uint32_t fromBuffer{kNone};
    /// Index into RenderGraph::delays, or kNone.
    std::uint32_t delayLine{kNone};
};

/// A step's place in the parallel plan. Indexed like RenderGraph::steps.
struct ParallelStep {
    /// Per input port, what is summed into it: [gatherFirst, +gatherCount) into
    /// ParallelPlan::gathers, in exactly the order the serial walk adds the same edges,
    /// so every accumulator's bits are the same whichever thread sums it.
    std::array<std::uint32_t, kMaxInputPorts> gatherFirst{0, 0};
    std::array<std::uint32_t, kMaxInputPorts> gatherCount{0, 0};
    /// Where the node writes, per output port, in ParallelPlan::buffers.
    std::array<std::uint32_t, kMaxOutputPorts> outputBuffers{kNone, kNone};
    /// Steps that may start only once this one is done: [firstSuccessor,
    /// +successorCount) into ParallelPlan::successors.
    std::uint32_t firstSuccessor{0};
    std::uint32_t successorCount{0};
    /// How many steps must finish before this one may start.
    std::uint32_t predecessors{0};
};

/// The same steps as a dependency graph, for running a piece on several threads
/// (Scheduler, Crew.h). Empty when the builder was not asked for one; the serial walk
/// over RenderGraph::steps is then the only way to run the graph.
///
/// What differs from the serial plan, and why the output is bit for bit the same:
///   - a step pulls its inputs when it starts rather than each producer pushing into
///     an accumulator when it finishes. The additions into one accumulator are the
///     same additions in the same order (GatherStep), so the sum is the same float;
///   - every output port has a buffer of its own. The serial plan shares buffers
///     between steps whose lifetimes do not overlap in the *serial* order, which says
///     nothing once steps overlap in time;
///   - an accumulator is the running thread's scratch, alive only while its step runs;
///   - dependencies that are not audio are edges too: a controller and the node whose
///     parameter it writes, and two steps that share an event cursor or a node, keep
///     the order the serial walk gives them.
struct ParallelPlan {
    std::span<const ParallelStep> steps;
    std::span<const GatherStep> gathers;
    std::span<const std::uint32_t> successors;
    /// Steps with no predecessors, in step order.
    std::span<const std::uint32_t> roots;
    BufferPool buffers;
    /// The master's post-fader buffer in `buffers`.
    std::uint32_t outputBuffer{kNone};
    /// Per step, predecessors still running in the piece being run. Reset at the start
    /// of every piece; written only by whoever is running the graph.
    std::span<std::atomic<std::uint32_t>> pending;
    /// The ready queue: each step's index + 1 once it may run, 0 before. Reset per piece.
    std::span<std::atomic<std::uint32_t>> ready;

    [[nodiscard]] bool available() const noexcept {
        return !steps.empty() && pending.size() == steps.size() && ready.size() == steps.size();
    }
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
    /// The parallel plan, when one was built.
    ParallelPlan parallel;
};

} // namespace adx::graph
