// adx-thread: realtime
//
// A channel that has been removed, still ringing (phase_7.md §4.4: "Channel removed |
// its voices release, not cut").
//
// When a reload - or an undo, or a script - removes a channel, its node leaves the
// model, but its voices may be mid-note. The graph builder wraps the old node in one of
// these and keeps it feeding the insert the channel fed: its first block releases every
// voice, and it renders their release, with the parameters the channel last had, until
// they are silent or kInstrumentReleaseCapMs has passed. Then it is silent, and the next
// rebuild leaves it out.
//
// It takes no events and no parameters of its own: the snapshot has none for a channel
// that no longer exists. It holds the channel by pointer: NodeStore owns it, on the main
// thread, for as long as this node is in a graph.
#pragma once

#include <atomic>
#include <cstdint>

#include "engine/graph/Node.h"
#include "engine/graph/nodes/ChannelNode.h"

namespace adx::graph {

class DepartingNode final : public Node {
public:
    DepartingNode(ChannelNode& channel, std::uint32_t capFrames) noexcept;

    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& context) noexcept override;
    void reset() noexcept override {}
    [[nodiscard]] PortSpec ports() const noexcept override;

    /// True once silent for good. Any thread.
    [[nodiscard]] bool done() const noexcept {
        return m_done.load(std::memory_order_acquire);
    }

private:
    ChannelNode* m_channel;
    std::uint32_t m_remaining;
    std::atomic<bool> m_done{false};
};

} // namespace adx::graph
