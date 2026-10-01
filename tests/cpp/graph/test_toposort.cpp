// Execution order: deterministic, complete, dependencies first.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>

#include "engine/graph/Graph.h"
#include "engine/graph/TopoSort.h"
#include "engine/graph/nodes/SumNode.h"

using adx::graph::Graph;
using adx::graph::NodeId;
using adx::graph::SumNode;

namespace {

struct Edge {
    std::uint32_t from;
    std::uint32_t to;
};

Graph buildWithEdges(std::uint32_t nodes, const std::vector<Edge>& edges) {
    Graph graph;
    for (std::uint32_t i = 0; i < nodes; ++i) {
        static_cast<void>(graph.add(std::make_shared<SumNode>(), "n" + std::to_string(i)));
    }
    for (const Edge& edge : edges) {
        graph.connect(NodeId{edge.from}, 0, NodeId{edge.to}, 0);
    }
    return graph;
}

std::size_t positionOf(const std::vector<NodeId>& order, std::uint32_t id) {
    return static_cast<std::size_t>(std::ranges::find(order, NodeId{id}) - order.begin());
}

} // namespace

TEST_CASE("toposort_deterministic", "[graph][toposort]") {
    // A layered graph with a lot of freedom: 64 nodes, every node in a layer feeding
    // every node in the next. Kahn's algorithm has many valid answers here, and the
    // one it gives must not depend on the order the edges were added in - summation
    // order, and so the output's last bits, follow from it.
    std::vector<Edge> edges;
    constexpr std::uint32_t kLayers = 8;
    constexpr std::uint32_t kWidth = 8;
    for (std::uint32_t layer = 0; layer + 1 < kLayers; ++layer) {
        for (std::uint32_t a = 0; a < kWidth; ++a) {
            for (std::uint32_t b = 0; b < kWidth; ++b) {
                if ((a + b + layer) % 3 != 0) {
                    edges.push_back(
                        Edge{.from = (layer * kWidth) + a, .to = ((layer + 1) * kWidth) + b});
                }
            }
        }
    }

    const adx::graph::TopoResult reference =
        adx::graph::topoSort(buildWithEdges(kLayers * kWidth, edges));
    REQUIRE(reference.ok);
    REQUIRE(reference.order.size() == std::size_t{kLayers} * kWidth);

    std::mt19937 shuffle{12345};
    for (int build = 0; build < 1000; ++build) {
        std::ranges::shuffle(edges, shuffle);
        const adx::graph::TopoResult again =
            adx::graph::topoSort(buildWithEdges(kLayers * kWidth, edges));
        REQUIRE(again.ok);
        REQUIRE(again.order == reference.order);
    }
}

TEST_CASE("toposort_diamond", "[graph][toposort]") {
    //     0
    //    / \
    //   1   2
    //    \ /
    //     3
    const Graph graph = buildWithEdges(4, {Edge{.from = 0, .to = 1}, Edge{.from = 0, .to = 2},
                                           Edge{.from = 1, .to = 3}, Edge{.from = 2, .to = 3}});
    const adx::graph::TopoResult result = adx::graph::topoSort(graph);
    REQUIRE(result.ok);
    REQUIRE(result.order.size() == 4);

    for (std::uint32_t id = 0; id < 4; ++id) {
        CHECK(std::ranges::count(result.order, NodeId{id}) == 1);
    }
    CHECK(positionOf(result.order, 0) < positionOf(result.order, 1));
    CHECK(positionOf(result.order, 0) < positionOf(result.order, 2));
    CHECK(positionOf(result.order, 1) < positionOf(result.order, 3));
    CHECK(positionOf(result.order, 2) < positionOf(result.order, 3));
    // And the tie between 1 and 2 goes to the lower id.
    CHECK(positionOf(result.order, 1) < positionOf(result.order, 2));
}
