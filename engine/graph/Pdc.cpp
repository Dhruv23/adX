// adx-thread: main
#include "engine/graph/Pdc.h"

#include <algorithm>

namespace adx::graph {

std::uint32_t PdcPlan::delayedEdges() const noexcept {
    return static_cast<std::uint32_t>(
        std::ranges::count_if(edgeDelay, [](std::uint32_t delay) { return delay > 0; }));
}

PdcPlan computePdc(const Graph& graph, std::span<const NodeId> order) {
    const std::size_t count = graph.nodes().size();
    PdcPlan plan;
    plan.arrival.assign(count, 0);
    plan.latency.assign(count, 0);
    plan.edgeDelay.assign(graph.edges().size(), 0);

    for (const GraphNode& node : graph.nodes()) {
        plan.latency[node.id.v] = node.node != nullptr ? node.node->latencySamples() : 0;
    }

    // Incoming edges per node, so the longest-path pass visits each edge once.
    std::vector<std::vector<std::size_t>> incoming(count);
    for (std::size_t e = 0; e < graph.edges().size(); ++e) {
        incoming[graph.edges()[e].to.v].push_back(e);
    }

    // Longest path in topological order: a node's input is complete when the slowest
    // of its inputs has arrived (phase_3.md §4.6 step 2).
    for (const NodeId id : order) {
        std::uint32_t latest = 0;
        for (const std::size_t e : incoming[id.v]) {
            const NodeId from = graph.edges()[e].from;
            latest = std::max(latest, plan.arrival[from.v] + plan.latency[from.v]);
        }
        plan.arrival[id.v] = latest;
    }

    // Every early input waits for the slowest (step 3). A sidechain input is aligned
    // like any other: a key signal that leads its audio by a lookahead's worth is a
    // ducker that pumps early.
    for (std::size_t e = 0; e < graph.edges().size(); ++e) {
        const GraphEdge& edge = graph.edges()[e];
        const std::uint32_t ready = plan.arrival[edge.from.v] + plan.latency[edge.from.v];
        plan.edgeDelay[e] = plan.arrival[edge.to.v] - ready;
    }

    if (graph.hasOutput() && graph.output().v < count) {
        plan.totalLatency = plan.arrival[graph.output().v] + plan.latency[graph.output().v];
    }
    return plan;
}

} // namespace adx::graph
