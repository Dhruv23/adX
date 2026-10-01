// A routing cycle is refused before anything is published, and the refusal names it.

#include <catch2/catch_test_macros.hpp>

#include "engine/graph/Graph.h"
#include "engine/graph/GraphBuilder.h"
#include "engine/graph/TopoSort.h"
#include "engine/graph/nodes/SumNode.h"
#include "engine/project/Project.h"
#include "engine/project/SnapshotBuilder.h"
#include "tests/cpp/graph/GraphFixtures.h"

using adx::graph::Graph;
using adx::graph::kPortMain;
using adx::graph::kPortPost;
using adx::graph::kPortSidechain;

TEST_CASE("cycle_detected_with_path", "[graph][cycle]") {
    adx::project::Project project;
    std::vector<adx::core::InsertId> ids;
    for (int i = 0; i < 4; ++i) {
        adx::project::Insert insert;
        insert.id = project.newInsertId();
        insert.name = "I" + std::to_string(i);
        project.mixer.inserts.push_back(insert);
        ids.push_back(insert.id);
    }
    project.mixer.master = ids[0];
    // insert.2 -> insert.3 -> insert.4 -> insert.2, with insert.1 as the master.
    const auto route = [&project](adx::core::InsertId from, adx::core::InsertId to) {
        project.mixer.routes.push_back(
            adx::project::Route{.id = project.newRouteId(), .from = from, .to = to});
    };
    route(ids[1], ids[2]);
    route(ids[2], ids[3]);
    route(ids[3], ids[1]);

    adx::graph::NodeStore nodes{adx::graph::PrepareInfo{}};
    const adx::graph::GraphBuild build = adx::graph::buildGraph(project, nodes);
    REQUIRE_FALSE(build.ok);
    INFO(build.error);
    CHECK(build.error.find("insert.2") != std::string::npos);
    CHECK(build.error.find("insert.3") != std::string::npos);
    CHECK(build.error.find("insert.4") != std::string::npos);
    CHECK(build.error.find("insert.1") == std::string::npos);
    CHECK(build.error.find(" -> ") != std::string::npos);

    // And the builder refuses to produce a snapshot from it: nothing reaches the audio
    // thread.
    adx::project::SnapshotBuilder builder{48000, 2048};
    const adx::project::SnapshotBuildResult result =
        builder.build(project, 1, adx::project::dirty::kAll);
    CHECK(result.snapshot == nullptr);
    CHECK_FALSE(result.error.empty());
}

TEST_CASE("cycle_via_sidechain", "[graph][cycle]") {
    // source -> ducker(main) -> bus -> ducker(sidechain). The sidechain edge closes the
    // loop, and must be caught exactly like any other edge: a key signal read from
    // outside the sort order reads last block's data.
    Graph graph;
    const auto source = graph.add(std::make_shared<adx::graph::SumNode>(), "source");
    const auto ducker = graph.add(std::make_shared<adx::tests::SidechainNode>(), "insert.7");
    const auto bus = graph.add(std::make_shared<adx::graph::SumNode>(), "insert.9");
    graph.connect(source, kPortPost, ducker, kPortMain);
    graph.connect(ducker, kPortPost, bus, kPortMain);

    REQUIRE(adx::graph::topoSort(graph).ok);

    graph.connect(bus, kPortPost, ducker, kPortSidechain);
    const adx::graph::TopoResult result = adx::graph::topoSort(graph);
    REQUIRE_FALSE(result.ok);
    INFO(result.message);
    CHECK(result.message.find("insert.7") != std::string::npos);
    CHECK(result.message.find("insert.9") != std::string::npos);
    CHECK(result.message.find("source") == std::string::npos);
    REQUIRE(result.cycle.size() >= 3);
    CHECK(result.cycle.front() == result.cycle.back());
}
