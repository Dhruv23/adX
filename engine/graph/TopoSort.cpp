// adx-thread: main
#include "engine/graph/TopoSort.h"

#include <algorithm>
#include <functional>
#include <queue>

namespace adx::graph {
namespace {

/// Walks backwards from `start` along predecessors that are still unsorted until a
/// node repeats. Every unsorted node has at least one unsorted predecessor - that is
/// what being left over by Kahn means - so the walk always closes a loop.
std::vector<NodeId> findCycle(const Graph& graph, const std::vector<std::uint32_t>& inDegree,
                              std::uint32_t start) {
    const std::size_t count = graph.nodes().size();
    std::vector<std::vector<std::uint32_t>> predecessors(count);
    for (const GraphEdge& edge : graph.edges()) {
        predecessors[edge.to.v].push_back(edge.from.v);
    }
    for (auto& list : predecessors) {
        std::ranges::sort(list);
    }

    std::vector<std::size_t> seenAt(count, count);
    std::vector<std::uint32_t> walk;
    std::uint32_t current = start;
    while (seenAt[current] == count) {
        seenAt[current] = walk.size();
        walk.push_back(current);
        // The lowest-id unsorted predecessor, so the reported cycle is deterministic.
        std::uint32_t next = current;
        for (const std::uint32_t candidate : predecessors[current]) {
            if (inDegree[candidate] > 0) {
                next = candidate;
                break;
            }
        }
        current = next;
    }

    // walk[seenAt[current]..] is the loop, traversed against the edges. Reverse it so
    // it reads in signal-flow order, and close it.
    std::vector<NodeId> cycle;
    for (std::size_t i = walk.size(); i > seenAt[current]; --i) {
        cycle.push_back(NodeId{walk[i - 1]});
    }
    if (!cycle.empty()) {
        cycle.push_back(cycle.front());
    }
    return cycle;
}

std::string describe(const Graph& graph, const std::vector<NodeId>& cycle) {
    std::vector<std::string> labels;
    for (const NodeId id : cycle) {
        const GraphNode* node = graph.find(id);
        const std::string label = node != nullptr ? node->label : "node." + std::to_string(id.v);
        // An insert is several nodes; one hop through it is one name.
        if (labels.empty() || labels.back() != label) {
            labels.push_back(label);
        }
    }
    std::string message = "routing cycle: ";
    for (std::size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) {
            message += " -> ";
        }
        message += labels[i];
    }
    return message;
}

} // namespace

TopoResult topoSort(const Graph& graph) {
    const std::size_t count = graph.nodes().size();
    std::vector<std::uint32_t> inDegree(count, 0);
    std::vector<std::vector<std::uint32_t>> successors(count);
    for (const GraphEdge& edge : graph.edges()) {
        if (edge.from.v >= count || edge.to.v >= count) {
            continue;
        }
        successors[edge.from.v].push_back(edge.to.v);
        ++inDegree[edge.to.v];
    }

    // A min-heap on id: of everything ready, the lowest id runs next.
    std::priority_queue<std::uint32_t, std::vector<std::uint32_t>, std::greater<>> ready;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (inDegree[i] == 0) {
            ready.push(i);
        }
    }

    TopoResult result;
    result.order.reserve(count);
    while (!ready.empty()) {
        const std::uint32_t next = ready.top();
        ready.pop();
        result.order.push_back(NodeId{next});
        for (const std::uint32_t successor : successors[next]) {
            if (--inDegree[successor] == 0) {
                ready.push(successor);
            }
        }
    }

    if (result.order.size() == count) {
        result.ok = true;
        return result;
    }

    std::uint32_t start = 0;
    while (start < count && inDegree[start] == 0) {
        ++start;
    }
    result.order.clear();
    result.cycle = findCycle(graph, inDegree, start);
    result.message = describe(graph, result.cycle);
    return result;
}

} // namespace adx::graph
