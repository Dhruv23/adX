// adx-thread: main
//
// Plugin delay compensation.
//
// Every node declares the latency it introduces; this finds, for every edge, how long
// it must be held back so that everything arriving at a convergence point arrives
// aligned - and how late the whole graph's output is. Computed on the main thread and
// baked into the render graph as DelayLines: the audio thread never computes a
// latency (phase_3.md §4.6).
//
// In Phase 3 every node reports zero and no line is ever created. Lookahead limiters,
// convolution (Phase 4) and plugins (Phase 9) are what make this non-trivial; they
// declare latencySamples() and compensation happens without them doing anything else.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/graph/Graph.h"

namespace adx::graph {

struct PdcPlan {
    /// Per edge, in Graph::edges() order: samples to delay it by.
    std::vector<std::uint32_t> edgeDelay;
    /// Per node, by id: when its input is complete, in samples after the event that
    /// caused it. The longest path to it.
    std::vector<std::uint32_t> arrival;
    /// Per node, by id: its own latency, as reported when this plan was computed.
    std::vector<std::uint32_t> latency;
    /// arrival + latency at the output node. The latency readout, and what Phase 8's
    /// recording alignment subtracts.
    std::uint32_t totalLatency{0};

    /// How many edges need a delay line. Zero when every latency is zero.
    [[nodiscard]] std::uint32_t delayedEdges() const noexcept;
};

/// `order` must be a topological order of `graph` (TopoResult::order).
[[nodiscard]] PdcPlan computePdc(const Graph& graph, std::span<const NodeId> order);

} // namespace adx::graph
