// adx-thread: main
#include "engine/graph/Graph.h"

#include <utility>

namespace adx::graph {

NodeId Graph::add(std::shared_ptr<Node> node, std::string label) {
    const NodeId id{static_cast<std::uint32_t>(m_nodes.size())};
    m_nodes.push_back(GraphNode{.id = id, .node = std::move(node), .label = std::move(label)});
    return id;
}

void Graph::connect(NodeId from, std::uint32_t fromPort, NodeId to, std::uint32_t toPort) {
    m_edges.push_back(GraphEdge{.from = from, .fromPort = fromPort, .to = to, .toPort = toPort});
}

const GraphNode* Graph::find(NodeId id) const noexcept {
    return id.v < m_nodes.size() ? &m_nodes[id.v] : nullptr;
}

GraphNode* Graph::find(NodeId id) noexcept {
    return id.v < m_nodes.size() ? &m_nodes[id.v] : nullptr;
}

} // namespace adx::graph
