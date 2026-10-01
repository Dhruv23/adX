// adx-thread: main
//
// A graph as the main thread builds it: nodes, labelled for humans, and the edges
// between their ports.
//
// Main-thread only, and marked so: this header owns std::vector, std::string and
// std::shared_ptr, so the realtime ban list must never see it arrive in audio-thread
// code - and tools/lint.py's include walk guarantees it does not. What the audio
// thread gets is a RenderGraph compiled from this (CompiledGraph.h).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/graph/Node.h"
#include "engine/graph/NodeId.h"
#include "engine/graph/PortSpec.h"

namespace adx::graph {

struct GraphNode {
    NodeId id;
    std::shared_ptr<Node> node;
    /// What a cycle message calls this node: `insert.2`, `channel.Lead`. Every node an
    /// insert expands into carries the insert's label, so a cycle through a strip's
    /// slots and fader reads as one hop, not five.
    std::string label;
    std::uint32_t eventTrack{kNone};
    std::uint32_t timeSource{0};
    std::uint32_t paramBase{0};
    std::uint32_t paramCount{0};
};

struct GraphEdge {
    NodeId from;
    std::uint32_t fromPort{kPortPost};
    NodeId to;
    std::uint32_t toPort{kPortMain};

    [[nodiscard]] friend bool operator==(const GraphEdge&, const GraphEdge&) noexcept = default;
};

class Graph {
public:
    /// Adds a node and returns its id. Ids are handed out in call order, which is the
    /// order the topological sort breaks ties in - so the order nodes are added is the
    /// order they run in, wherever the dependencies leave a choice.
    NodeId add(std::shared_ptr<Node> node, std::string label);

    void connect(NodeId from, std::uint32_t fromPort, NodeId to, std::uint32_t toPort);

    /// The node whose post-fader output is the graph's output. Unset renders silence.
    void setOutput(NodeId id) noexcept {
        m_output = id;
        m_hasOutput = true;
    }

    [[nodiscard]] const std::vector<GraphNode>& nodes() const noexcept {
        return m_nodes;
    }
    [[nodiscard]] std::vector<GraphNode>& nodes() noexcept {
        return m_nodes;
    }
    [[nodiscard]] const std::vector<GraphEdge>& edges() const noexcept {
        return m_edges;
    }
    [[nodiscard]] bool hasOutput() const noexcept {
        return m_hasOutput;
    }
    [[nodiscard]] NodeId output() const noexcept {
        return m_output;
    }

    /// Nodes are stored by id - id v is at index v - so this is O(1).
    [[nodiscard]] const GraphNode* find(NodeId id) const noexcept;
    [[nodiscard]] GraphNode* find(NodeId id) noexcept;

private:
    std::vector<GraphNode> m_nodes;
    std::vector<GraphEdge> m_edges;
    NodeId m_output;
    bool m_hasOutput{false};
};

} // namespace adx::graph
