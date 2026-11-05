// adx-thread: main
#include "engine/graph/CompiledGraph.h"

#include <algorithm>
#include <array>
#include <numeric>
#include <utility>

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
CompiledGraph::CompiledGraph(const Graph& graph, const TopoResult& order, const PdcPlan& pdc,
                             bool parallel) {
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
        step.audioTrack = source.audioTrack;
        step.timeSource = source.timeSource;
        step.paramBase = source.paramBase;
        step.paramCount = source.paramCount;
        step.modulates = source.modulates;
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
    if (parallel && stepCount > 0) {
        buildParallel(graph, live, stepOf);
    }
}

// One straight line again: buffers, gathers, ordering edges, then the flat arrays.
// NOLINTNEXTLINE(readability-function-size)
void CompiledGraph::buildParallel(const Graph& graph, const std::vector<std::size_t>& live,
                                  const std::vector<std::uint32_t>& stepOf) {
    const std::size_t stepCount = m_steps.size();
    m_parallelSteps.assign(stepCount, ParallelStep{});

    // A buffer per output port: the serial plan gave one to every port a node has.
    std::uint32_t bufferCount = 0;
    for (std::size_t s = 0; s < stepCount; ++s) {
        for (std::uint32_t port = 0; port < kMaxOutputPorts; ++port) {
            if (m_steps[s].outputBuffers[port] != kNone) {
                m_parallelSteps[s].outputBuffers[port] = bufferCount++;
            }
        }
    }

    // Gathers per (step, input port). `live` is in the order the serial walk applies the
    // edges - by producing step, then consumer, port, source port and edge - and
    // m_edges[i] is live[i]. Filtered to one accumulator, that is the order its sum is
    // built in, so appending in this order keeps it.
    std::vector<std::vector<GatherStep>> gathers(stepCount * kMaxInputPorts);
    std::vector<std::vector<std::uint32_t>> after(stepCount);
    for (std::size_t i = 0; i < live.size(); ++i) {
        const GraphEdge& edge = graph.edges()[live[i]];
        const std::uint32_t from = stepOf[edge.from.v];
        const std::uint32_t to = stepOf[edge.to.v];
        gathers[(static_cast<std::size_t>(to) * kMaxInputPorts) + edge.toPort].push_back(
            GatherStep{.fromBuffer = m_parallelSteps[from].outputBuffers[edge.fromPort],
                       .delayLine = m_edges[i].delayLine});
        after[from].push_back(to);
    }

    // Order that is not audio, kept as the serial walk has it: the earlier step first.
    const auto keepOrder = [&after](std::uint32_t a, std::uint32_t b) {
        if (a != b) {
            after[std::min(a, b)].push_back(std::max(a, b));
        }
    };
    for (std::uint32_t c = 0; c < stepCount; ++c) {
        const std::uint32_t target = m_steps[c].modulates;
        if (target == kNone) {
            continue;
        }
        // A controller writes one parameter after it runs. Whoever reads that parameter,
        // and any other controller writing it, sees the value the serial order gives.
        for (std::uint32_t s = 0; s < stepCount; ++s) {
            const NodeStep& step = m_steps[s];
            const bool reads = step.paramCount > 0 && target >= step.paramBase &&
                               target - step.paramBase < step.paramCount;
            if (reads || step.modulates == target) {
                keepOrder(c, s);
            }
        }
    }
    // An event cursor advances as its step reads it, and a node keeps state between
    // calls: two steps sharing either run in step order.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> trackLast;
    std::vector<std::pair<const Node*, std::uint32_t>> nodeLast;
    for (std::uint32_t s = 0; s < stepCount; ++s) {
        const NodeStep& step = m_steps[s];
        if (step.eventTrack != kNone) {
            const auto found = std::ranges::find_if(
                trackLast, [&step](const auto& entry) { return entry.first == step.eventTrack; });
            if (found != trackLast.end()) {
                keepOrder(found->second, s);
                found->second = s;
            } else {
                trackLast.emplace_back(step.eventTrack, s);
            }
        }
        if (step.node != nullptr) {
            const auto found = std::ranges::find_if(
                nodeLast, [&step](const auto& entry) { return entry.first == step.node; });
            if (found != nodeLast.end()) {
                keepOrder(found->second, s);
                found->second = s;
            } else {
                nodeLast.emplace_back(step.node, s);
            }
        }
    }

    for (std::uint32_t s = 0; s < stepCount; ++s) {
        std::vector<std::uint32_t>& next = after[s];
        std::ranges::sort(next);
        const auto duplicates = std::ranges::unique(next);
        next.erase(duplicates.begin(), duplicates.end());
        ParallelStep& step = m_parallelSteps[s];
        step.firstSuccessor = static_cast<std::uint32_t>(m_successors.size());
        step.successorCount = static_cast<std::uint32_t>(next.size());
        m_successors.insert(m_successors.end(), next.begin(), next.end());
        for (const std::uint32_t t : next) {
            ++m_parallelSteps[t].predecessors;
        }
        for (std::uint32_t port = 0; port < kMaxInputPorts; ++port) {
            const std::vector<GatherStep>& list =
                gathers[(static_cast<std::size_t>(s) * kMaxInputPorts) + port];
            step.gatherFirst[port] = static_cast<std::uint32_t>(m_gathers.size());
            step.gatherCount[port] = static_cast<std::uint32_t>(list.size());
            m_gathers.insert(m_gathers.end(), list.begin(), list.end());
        }
    }
    for (std::uint32_t s = 0; s < stepCount; ++s) {
        if (m_parallelSteps[s].predecessors == 0) {
            m_roots.push_back(s);
        }
    }

    m_parallelStorage.assign(static_cast<std::size_t>(bufferCount) * kFloatsPerBuffer, 0.0F);
    m_pending = std::vector<std::atomic<std::uint32_t>>(stepCount);
    m_ready = std::vector<std::atomic<std::uint32_t>>(stepCount);

    ParallelPlan& plan = m_view.parallel;
    plan.steps = m_parallelSteps;
    plan.gathers = m_gathers;
    plan.successors = m_successors;
    plan.roots = m_roots;
    plan.buffers = BufferPool{m_parallelStorage, bufferCount};
    plan.pending = m_pending;
    plan.ready = m_ready;
    if (graph.hasOutput() && graph.output().v < stepOf.size() &&
        stepOf[graph.output().v] != kNone) {
        plan.outputBuffer = m_parallelSteps[stepOf[graph.output().v]].outputBuffers[kPortPost];
    }
}

} // namespace adx::graph
