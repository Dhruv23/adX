// adx-thread: main
#include "engine/graph/CompiledGraph.h"

#include <algorithm>
#include <array>
#include <numeric>

namespace adx::graph {
namespace {

/// A buffer the plan needs, before it knows which physical buffer it gets.
struct Want {
    LiveInterval interval;
    std::uint32_t step{0};
    std::uint32_t port{0};
    bool accumulator{false};
};

} // namespace

// Long, but one straight line: collect lifetimes, colour them, lay the steps out. The
// parts share too much state to split without passing it all around.
// NOLINTNEXTLINE(readability-function-size)
CompiledGraph::CompiledGraph(const Graph& graph, const TopoResult& order, const PdcPlan& pdc) {
    const std::size_t nodeCount = graph.nodes().size();
    const std::size_t stepCount = order.order.size();

    std::vector<std::uint32_t> stepOf(nodeCount, kNone);
    std::vector<PortSpec> ports(nodeCount);
    for (std::size_t s = 0; s < stepCount; ++s) {
        stepOf[order.order[s].v] = static_cast<std::uint32_t>(s);
    }
    for (const GraphNode& node : graph.nodes()) {
        ports[node.id.v] = node.node != nullptr ? node.node->ports() : PortSpec{};
    }

    // Which edges are real: both ends exist, the source has the port it leaves from,
    // the destination has the port it arrives at. Anything else is dropped here, so
    // nothing downstream has to ask again.
    std::vector<std::size_t> live;
    for (std::size_t e = 0; e < graph.edges().size(); ++e) {
        const GraphEdge& edge = graph.edges()[e];
        if (edge.from.v < nodeCount && edge.to.v < nodeCount && stepOf[edge.from.v] != kNone &&
            stepOf[edge.to.v] != kNone && edge.fromPort < ports[edge.from.v].outputs &&
            edge.toPort < ports[edge.to.v].inputs) {
            live.push_back(e);
        }
    }

    // --- lifetimes ---------------------------------------------------------------
    // An accumulator lives from its first producer's step to its consumer's. An output
    // lives only for its own step: edges are applied the moment the node finishes, so
    // nothing reads it later - except the graph output, which the scheduler copies to
    // the device after every step has run.
    std::vector<Want> wants;
    std::vector<std::array<std::uint32_t, kMaxInputPorts>> firstProducer(
        nodeCount, std::array<std::uint32_t, kMaxInputPorts>{kNone, kNone});
    for (const std::size_t e : live) {
        const GraphEdge& edge = graph.edges()[e];
        std::uint32_t& first = firstProducer[edge.to.v][edge.toPort];
        first = std::min(first, stepOf[edge.from.v]);
    }
    for (std::uint32_t s = 0; s < stepCount; ++s) {
        const NodeId id = order.order[s];
        for (std::uint32_t port = 0; port < ports[id.v].inputs && port < kMaxInputPorts; ++port) {
            if (firstProducer[id.v][port] != kNone) {
                wants.push_back(Want{.interval = {.first = firstProducer[id.v][port], .last = s},
                                     .step = s,
                                     .port = port,
                                     .accumulator = true});
            }
        }
        for (std::uint32_t port = 0; port < ports[id.v].outputs && port < kMaxOutputPorts; ++port) {
            const bool isOutput = graph.hasOutput() && graph.output() == id && port == kPortPost;
            const auto last = isOutput ? static_cast<std::uint32_t>(stepCount) : s;
            wants.push_back(Want{.interval = {.first = s, .last = last},
                                 .step = s,
                                 .port = port,
                                 .accumulator = false});
        }
    }
    // Sorted by start for first-fit; the rest of the key only makes the order total, so
    // the assignment - and with it every buffer index - is a pure function of the graph.
    std::ranges::sort(wants, [](const Want& a, const Want& b) {
        if (a.interval.first != b.interval.first) {
            return a.interval.first < b.interval.first;
        }
        if (a.step != b.step) {
            return a.step < b.step;
        }
        if (a.accumulator != b.accumulator) {
            return a.accumulator;
        }
        return a.port < b.port;
    });

    std::vector<LiveInterval> intervals;
    intervals.reserve(wants.size());
    for (const Want& want : wants) {
        intervals.push_back(want.interval);
    }
    std::vector<std::uint32_t> assignment(wants.size(), kNone);
    std::vector<std::uint32_t> busyUntil(wants.size(), 0);
    m_bufferCount = assignBuffers(intervals, assignment, busyUntil);

    // --- steps -------------------------------------------------------------------
    m_steps.resize(stepCount);
    std::vector<std::vector<std::uint32_t>> clearsAt(stepCount);
    for (std::size_t w = 0; w < wants.size(); ++w) {
        const Want& want = wants[w];
        NodeStep& step = m_steps[want.step];
        if (want.accumulator) {
            step.inputBuffers[want.port] = assignment[w];
            // Zeroed just before its first producer runs, which is the start of its
            // interval - so the zeroing cannot land on a buffer still in use.
            clearsAt[want.interval.first].push_back(assignment[w]);
        } else {
            step.outputBuffers[want.port] = assignment[w];
        }
    }

    for (std::uint32_t s = 0; s < stepCount; ++s) {
        const GraphNode& source = *graph.find(order.order[s]);
        NodeStep& step = m_steps[s];
        step.node = source.node.get();
        step.id = source.id;
        step.eventTrack = source.eventTrack;
        step.timeSource = source.timeSource;
        step.paramBase = source.paramBase;
        step.paramCount = source.paramCount;
        m_nodes.push_back(source.node);

        step.firstClear = static_cast<std::uint32_t>(m_clears.size());
        step.clearCount = static_cast<std::uint32_t>(clearsAt[s].size());
        m_clears.insert(m_clears.end(), clearsAt[s].begin(), clearsAt[s].end());
    }

    // --- edges -------------------------------------------------------------------
    // Grouped by the step that applies them, and within a step in a fixed order, so
    // each accumulator's summation order is decided here, once.
    std::ranges::sort(live, [&](std::size_t a, std::size_t b) {
        const GraphEdge& lhs = graph.edges()[a];
        const GraphEdge& rhs = graph.edges()[b];
        if (stepOf[lhs.from.v] != stepOf[rhs.from.v]) {
            return stepOf[lhs.from.v] < stepOf[rhs.from.v];
        }
        if (stepOf[lhs.to.v] != stepOf[rhs.to.v]) {
            return stepOf[lhs.to.v] < stepOf[rhs.to.v];
        }
        if (lhs.toPort != rhs.toPort) {
            return lhs.toPort < rhs.toPort;
        }
        if (lhs.fromPort != rhs.fromPort) {
            return lhs.fromPort < rhs.fromPort;
        }
        return a < b;
    });

    std::size_t delayFloats = 0;
    for (const std::size_t e : live) {
        delayFloats += DelayLine::storageFor(pdc.edgeDelay[e]);
    }
    m_delayStorage.assign(delayFloats, 0.0F);

    std::size_t delayOffset = 0;
    for (const std::size_t e : live) {
        const GraphEdge& edge = graph.edges()[e];
        NodeStep& from = m_steps[stepOf[edge.from.v]];
        if (from.edgeCount == 0) {
            from.firstEdge = static_cast<std::uint32_t>(m_edges.size());
        }
        ++from.edgeCount;

        EdgeStep step{.fromPort = edge.fromPort,
                      .toBuffer = m_steps[stepOf[edge.to.v]].inputBuffers[edge.toPort]};
        const std::uint32_t delay = pdc.edgeDelay[e];
        if (delay > 0) {
            step.delayLine = static_cast<std::uint32_t>(m_delays.size());
            DelayLine line;
            line.attach(
                std::span<float>{m_delayStorage}.subspan(delayOffset, DelayLine::storageFor(delay)),
                delay);
            m_delays.push_back(line);
            delayOffset += DelayLine::storageFor(delay);
        }
        m_edges.push_back(step);
    }

    m_bufferStorage.assign(static_cast<std::size_t>(m_bufferCount) * kFloatsPerBuffer, 0.0F);

    m_view.steps = m_steps;
    m_view.edges = m_edges;
    m_view.clears = m_clears;
    m_view.buffers = BufferPool{m_bufferStorage, m_bufferCount};
    m_view.delays = m_delays;
    m_view.totalLatency = pdc.totalLatency;
    if (graph.hasOutput() && graph.output().v < nodeCount && stepOf[graph.output().v] != kNone) {
        m_view.outputBuffer = m_steps[stepOf[graph.output().v]].outputBuffers[kPortPost];
    }
}

} // namespace adx::graph
