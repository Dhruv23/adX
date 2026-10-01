// Hand-built graphs, run through the real scheduler.
//
// The graph tests need shapes no project produces - a node with 512 samples of
// latency, a sidechain port, an impulse - so these build a Graph directly, compile it
// exactly as the engine does, wrap it in a Snapshot, and call Scheduler::render.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/graph/CompiledGraph.h"
#include "engine/graph/Graph.h"
#include "engine/graph/Node.h"
#include "engine/graph/Scheduler.h"
#include "engine/graph/nodes/ChannelNode.h"
#include "engine/project/Snapshot.h"
#include "engine/rt/BlockArena.h"
#include "engine/transport/TransportSet.h"

namespace adx::tests {

/// One sample of 1.0 at absolute frame `at`, silence elsewhere.
class ImpulseNode final : public graph::Node {
public:
    explicit ImpulseNode(std::uint64_t at) noexcept : m_at(at) {}
    void prepare(const graph::PrepareInfo& /*info*/) override {}
    void process(graph::ProcessContext& context) noexcept override;
    void reset() noexcept override {}
    [[nodiscard]] graph::PortSpec ports() const noexcept override {
        return graph::PortSpec{.inputs = 0, .outputs = 1};
    }

private:
    std::uint64_t m_at;
    std::uint64_t m_frame{0};
};

/// Delays its input by exactly `latency` samples and says so.
class LatencyNode final : public graph::Node {
public:
    explicit LatencyNode(std::uint32_t latency) : m_latency(latency) {}
    void prepare(const graph::PrepareInfo& /*info*/) override;
    void process(graph::ProcessContext& context) noexcept override;
    void reset() noexcept override {}
    [[nodiscard]] std::uint32_t latencySamples() const noexcept override {
        return m_latency;
    }
    [[nodiscard]] graph::PortSpec ports() const noexcept override {
        return graph::PortSpec{.inputs = 1, .outputs = 1};
    }

private:
    std::uint32_t m_latency;
    std::vector<float> m_left;
    std::vector<float> m_right;
    std::size_t m_position{0};
};

/// Main + sidechain in, main passed through. The shape a ducker has.
class SidechainNode final : public graph::Node {
public:
    void prepare(const graph::PrepareInfo& /*info*/) override {}
    void process(graph::ProcessContext& context) noexcept override;
    void reset() noexcept override {}
    [[nodiscard]] graph::PortSpec ports() const noexcept override {
        return graph::PortSpec{.inputs = 2, .outputs = 1};
    }
};

/// A Graph compiled and wrapped in a Snapshot, with everything the snapshot's spans
/// point into.
struct ManualRender {
    std::unique_ptr<graph::CompiledGraph> compiled;
    std::vector<project::EventTrack> tracks;
    std::vector<std::vector<project::ScheduledEvent>> events;
    std::vector<project::EventCursor> cursors;
    std::vector<float> params;
    std::vector<core::TempoEvent> tempoEvents{core::TempoEvent{}};
    std::vector<double> cumSeconds{0.0};
    project::Snapshot snapshot;
    std::vector<std::byte> arenaStorage = std::vector<std::byte>(1U << 20U);
    rt::BlockArena arena{arenaStorage.data(), arenaStorage.size()};
    transport::TransportSet transport;
    graph::Scheduler scheduler;

    /// Compiles `graph` (which must be acyclic) and wires the snapshot.
    void compile(const graph::Graph& graph);

    /// Renders `frames` in blocks of `block` and returns interleaved stereo.
    std::vector<float> render(std::uint64_t frames, std::uint32_t block);
};

/// Drives one node directly: `events` at their offsets, `frames` long, and returns
/// its stereo output interleaved. For voice tests that want no graph around the node.
std::vector<float> runChannelNode(graph::ChannelNode& node, std::vector<graph::BlockEvent> events,
                                  std::uint32_t frames, std::vector<float> params = {1, 0, 1, 0});

} // namespace adx::tests
