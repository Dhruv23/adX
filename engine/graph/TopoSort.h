// adx-thread: main
//
// Execution order, and the cycle that prevents one.
//
// Kahn's algorithm with ties broken by ascending NodeId. The tie-break is not
// cosmetic: when several nodes are ready, which one runs first decides the order
// their outputs are summed downstream, and summing floats in a different order
// changes the last bits - which breaks bit-identical offline rendering and every
// golden hash. A two-line detail that is very expensive to discover late
// (phase_3.md §4.4).
#pragma once

#include <string>
#include <vector>

#include "engine/graph/Graph.h"

namespace adx::graph {

struct TopoResult {
    bool ok{false};
    /// Every node, dependencies first. Empty on failure.
    std::vector<NodeId> order;
    /// On failure: one cycle, as the nodes along it, first node repeated at the end.
    std::vector<NodeId> cycle;
    /// On failure: the cycle by label - `insert.2 -> insert.5 -> insert.2`. "Routing
    /// cycle detected" with no path is unactionable.
    std::string message;
};

[[nodiscard]] TopoResult topoSort(const Graph& graph);

} // namespace adx::graph
