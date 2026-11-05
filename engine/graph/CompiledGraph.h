// adx-thread: main
//
// A Graph compiled into a RenderGraph, plus everything the RenderGraph's spans point
// into.
//
// Compilation decides, once, on the main thread: the order the steps run in, which
// accumulator every edge feeds, which buffers can be shared because their lifetimes
// do not overlap, which accumulators need zeroing and when, and which edges need a
// delay line. What the audio thread gets is RenderGraph - arrays it walks front to
// back.
//
// Not listed in phase_3.md §3's manifest; the plan folds this into Graph.cpp. It is
// its own file because it is the one place that turns an owning, main-thread
// structure into the audio thread's non-owning view, and that seam deserves to be
// visible (§10 of the plan).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/graph/Graph.h"
#include "engine/graph/Pdc.h"
#include "engine/graph/RenderGraph.h"
#include "engine/graph/TopoSort.h"

namespace adx::graph {

class CompiledGraph {
public:
    /// Compiles `graph`. `order` and `pdc` must have been computed from it. Nodes are
    /// shared, not copied: a node that survives from the previous graph keeps its
    /// voices, its meter ring and its tails.
    ///
    /// `parallel` also builds the ParallelPlan (RenderGraph.h), for an engine that runs
    /// pieces on several threads. It costs a buffer per output port, so a builder whose
    /// engine has no helpers leaves it off.
    CompiledGraph(const Graph& graph, const TopoResult& order, const PdcPlan& pdc,
                  bool parallel = false);

    CompiledGraph(const CompiledGraph&) = delete;
    CompiledGraph& operator=(const CompiledGraph&) = delete;
    CompiledGraph(CompiledGraph&&) = delete;
    CompiledGraph& operator=(CompiledGraph&&) = delete;
    ~CompiledGraph() = default;

    /// The audio thread's view. Valid for this object's lifetime.
    [[nodiscard]] const RenderGraph& view() const noexcept {
        return m_view;
    }

    /// Distinct audio buffers the plan needed. What bus_count_dynamic asserts on, and
    /// the evidence that a 200-strip project did not need 200 x (in + out) buffers.
    [[nodiscard]] std::uint32_t bufferCount() const noexcept {
        return m_bufferCount;
    }
    [[nodiscard]] std::size_t delayLineCount() const noexcept {
        return m_delays.size();
    }
    [[nodiscard]] std::size_t stepCount() const noexcept {
        return m_steps.size();
    }
    [[nodiscard]] const std::vector<NodeStep>& steps() const noexcept {
        return m_steps;
    }

private:
    void buildParallel(const Graph& graph, const std::vector<std::size_t>& live,
                       const std::vector<std::uint32_t>& stepOf);

    std::vector<std::shared_ptr<Node>> m_nodes;
    std::vector<NodeStep> m_steps;
    std::vector<EdgeStep> m_edges;
    std::vector<std::uint32_t> m_clears;
    std::vector<float> m_bufferStorage;
    std::vector<float> m_delayStorage;
    std::vector<DelayLine> m_delays;
    std::uint32_t m_bufferCount{0};

    // The parallel plan's arrays, empty unless it was asked for.
    std::vector<ParallelStep> m_parallelSteps;
    std::vector<GatherStep> m_gathers;
    std::vector<std::uint32_t> m_successors;
    std::vector<std::uint32_t> m_roots;
    std::vector<float> m_parallelStorage;
    std::vector<std::atomic<std::uint32_t>> m_pending;
    std::vector<std::atomic<std::uint32_t>> m_ready;

    RenderGraph m_view;
};

} // namespace adx::graph
