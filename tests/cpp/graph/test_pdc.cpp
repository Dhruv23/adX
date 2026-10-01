// Plugin delay compensation: parallel paths arrive together, and the total is reported.

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "engine/graph/Graph.h"
#include "engine/graph/GraphBuilder.h"
#include "engine/graph/Pdc.h"
#include "engine/graph/TopoSort.h"
#include "engine/graph/nodes/SumNode.h"
#include "tests/cpp/graph/GraphFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::graph::Graph;
using adx::graph::kPortMain;
using adx::graph::kPortPost;

TEST_CASE("pdc_aligns_parallel_paths", "[graph][pdc]") {
    // impulse -> fast (0) -> sum
    //         -> slow (512) -> sum -> out
    Graph graph;
    const auto impulse = graph.add(std::make_shared<adx::tests::ImpulseNode>(100), "impulse");
    const auto fast = graph.add(std::make_shared<adx::tests::LatencyNode>(0), "fast");
    const auto slow = graph.add(std::make_shared<adx::tests::LatencyNode>(512), "slow");
    const auto sum = graph.add(std::make_shared<adx::graph::SumNode>(), "sum");
    graph.connect(impulse, kPortPost, fast, kPortMain);
    graph.connect(impulse, kPortPost, slow, kPortMain);
    graph.connect(fast, kPortPost, sum, kPortMain);
    graph.connect(slow, kPortPost, sum, kPortMain);
    graph.setOutput(sum);
    for (auto& node : graph.nodes()) {
        node.node->prepare(adx::graph::PrepareInfo{});
    }

    adx::tests::ManualRender rig;
    rig.compile(graph);
    CHECK(rig.compiled->delayLineCount() == 1);
    const std::vector<float> out = rig.render(4096, 256);

    // Cross-correlate the output against the impulse: aligned paths put all the
    // energy at one lag, 512, at twice the amplitude. Unaligned, it would be split
    // across lags 0 and 512.
    std::size_t bestLag = 0;
    float best = 0.0F;
    for (std::size_t lag = 0; lag < 2048; ++lag) {
        const float correlation = out[(100 + lag) * 2];
        if (std::abs(correlation) > std::abs(best)) {
            best = correlation;
            bestLag = lag;
        }
    }
    CHECK(bestLag == 512);
    CHECK(best == 2.0F);
    std::size_t nonZero = 0;
    for (std::size_t frame = 0; frame < 4096; ++frame) {
        nonZero += out[frame * 2] != 0.0F ? 1U : 0U;
    }
    CHECK(nonZero == 1);
}

TEST_CASE("pdc_reports_total_latency", "[graph][pdc]") {
    // a(100) -> b(200) -> sum     (300 along the top)
    // a(100) -> c(50)  -> sum     (150 along the bottom)
    Graph graph;
    const auto source = graph.add(std::make_shared<adx::tests::ImpulseNode>(0), "source");
    const auto a = graph.add(std::make_shared<adx::tests::LatencyNode>(100), "a");
    const auto b = graph.add(std::make_shared<adx::tests::LatencyNode>(200), "b");
    const auto c = graph.add(std::make_shared<adx::tests::LatencyNode>(50), "c");
    const auto sum = graph.add(std::make_shared<adx::graph::SumNode>(), "sum");
    graph.connect(source, kPortPost, a, kPortMain);
    graph.connect(a, kPortPost, b, kPortMain);
    graph.connect(a, kPortPost, c, kPortMain);
    graph.connect(b, kPortPost, sum, kPortMain);
    graph.connect(c, kPortPost, sum, kPortMain);
    graph.setOutput(sum);

    const adx::graph::TopoResult order = adx::graph::topoSort(graph);
    REQUIRE(order.ok);
    const adx::graph::PdcPlan plan = adx::graph::computePdc(graph, order.order);
    CHECK(plan.totalLatency == 300);
    CHECK(plan.arrival[sum.v] == 300);
    // The short path is the one held back, by exactly the difference.
    CHECK(plan.delayedEdges() == 1);
    CHECK(plan.edgeDelay.back() == 150);
}

TEST_CASE("pdc_zero_latency_inserts_nothing", "[graph][pdc]") {
    adx::tests::SyntheticSpec spec;
    spec.channels = 24;
    spec.notes = 200;
    const adx::project::Project project = adx::tests::syntheticProject(spec);

    adx::graph::NodeStore nodes{adx::graph::PrepareInfo{}};
    const adx::graph::GraphBuild build = adx::graph::buildGraph(project, nodes);
    REQUIRE(build.ok);
    CHECK(build.pdc.delayedEdges() == 0);
    CHECK(build.pdc.totalLatency == 0);
    const adx::graph::CompiledGraph compiled{build.graph, build.order, build.pdc};
    CHECK(compiled.delayLineCount() == 0);
}
