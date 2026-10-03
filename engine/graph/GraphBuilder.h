// adx-thread: main
//
// Project -> Graph.
//
//   one ChannelNode per Channel, feeding its output insert
//   per Insert: its slots in ascending id order, then its fader, then a meter
//   one SendNode per Send, tapping the fader's pre or post output
//   one edge per Route, fader to the target's first node
//
// Nodes come from a NodeStore keyed by entity, so a rebuild hands back the *same*
// node for an entity that survived - its voices keep sounding, its meter ring keeps
// its history, a reverb's tail keeps ringing - and only nodes for new entities are
// created and prepared. A structural edit therefore does not click.
//
// Cycle detection happens here, before anything is published, on the full graph
// including sidechain edges - which the model's own Validate cannot see, because a
// sidechain is a property of the effect that owns the port (phase_3.md §4.4).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/graph/Graph.h"
#include "engine/graph/Pdc.h"
#include "engine/graph/TopoSort.h"
#include "engine/graph/nodes/MeterNode.h"
#include "engine/project/ParamDescriptor.h"
#include "engine/project/ParamRef.h"
#include "engine/project/Snapshot.h"

namespace adx::format {
class SamplePool;
}

namespace adx::project {
class Project;
struct Channel;
struct Resources;
struct Slot;
} // namespace adx::project

namespace adx::graph {

/// Where one parameter slot's value comes from in the model. Most are a ParamRef the
/// UI can turn; Audible and Polarity are derived - mute and solo resolved into one
/// gain - so the audio thread never evaluates solo logic.
enum class ParamSource : std::uint8_t {
    ChannelVolume,
    ChannelPan,
    ChannelAudible,
    ChannelPitch,
    ChannelInstrument,
    InsertGain,
    InsertPan,
    InsertWidth,
    InsertAudible,
    InsertPolarity,
    SlotMix,
    SlotBypass,
    SlotEffect,
    SendLevel,
};

/// `index` for an instrument or effect parameter the model does not set: the slot
/// then holds the descriptor's default, and nothing can automate it until it is set.
inline constexpr std::uint16_t kAbsentParam = 0xFFFF;

struct ParamBinding {
    ParamSource source{ParamSource::ChannelVolume};
    std::uint32_t owner{0};
    /// For an instrument or effect parameter: its position in the model's named list,
    /// or kAbsentParam.
    std::uint16_t index{0};
    /// Which component of that parameter's curve this slot carries, if any
    /// (ParamDescriptor.h, CurvePart).
    project::CurvePart part{project::CurvePart::None};
    /// The value when the model has none: the descriptor's default.
    float fallback{0.0F};
};

/// The shape of a snapshot's parameter storage: one binding per slot, and the sorted
/// index a knob turn is looked up in. A function of the graph's structure only, so it
/// is reused whenever the graph is.
struct ParamLayout {
    std::vector<ParamBinding> bindings;
    std::vector<project::ParamSlot> index;
};

/// Resolves every binding against the project. O(parameters), main thread.
void fillParams(const project::Project& project, const ParamLayout& layout,
                std::vector<float>& out);

class NodeStore {
public:
    explicit NodeStore(PrepareInfo info) noexcept : m_info(info) {}

    /// Forgets every node if the rate or block ceiling changed: a node prepared for
    /// 44.1 kHz is wrong at 48.
    void setPrepareInfo(PrepareInfo info);
    [[nodiscard]] const PrepareInfo& prepareInfo() const noexcept {
        return m_info;
    }

    [[nodiscard]] std::shared_ptr<Node> channel(const project::Channel& channel,
                                                const project::Resources& resources);

    /// The pool instruments take their samples from. Null (the default) is
    /// SamplePool::global(); a test may give a private one.
    void setSamplePool(format::SamplePool* pool) noexcept {
        m_pool = pool;
    }
    [[nodiscard]] format::SamplePool* samplePool() const noexcept {
        return m_pool;
    }
    [[nodiscard]] std::shared_ptr<Node> fader(core::InsertId id);
    [[nodiscard]] std::shared_ptr<Node> meter(core::InsertId id, bool master);
    [[nodiscard]] std::shared_ptr<Node> slot(const project::Slot& slot);
    [[nodiscard]] std::shared_ptr<Node> send(core::SendId id);

    /// The meter for an insert, for the UI. Null when there is none.
    [[nodiscard]] std::shared_ptr<const MeterNode> findMeter(core::InsertId id) const;
    /// Every insert's meter, in insert-id order: what one UI frame reads (P3-7).
    void forEachMeter(const std::function<void(core::InsertId, const MeterNode&)>& visit) const;

    /// Drops every node not handed out since the last call. Called after each build,
    /// so a deleted insert's nodes are released once no snapshot uses them.
    void pruneUnused();

    [[nodiscard]] std::size_t size() const noexcept {
        return m_nodes.size();
    }
    /// Nodes created (and prepared) since construction. What node reuse is measured by.
    [[nodiscard]] std::uint64_t created() const noexcept {
        return m_created;
    }

private:
    enum class Kind : std::uint8_t { Channel, Fader, Meter, Slot, Send };
    using Key = std::pair<Kind, std::uint32_t>;

    struct Entry {
        std::shared_ptr<Node> node;
        bool used{false};
    };

    template<class Make> std::shared_ptr<Node> getOrMake(Key key, Make make);

    PrepareInfo m_info;
    format::SamplePool* m_pool{nullptr};
    std::map<Key, Entry> m_nodes;
    std::uint64_t m_created{0};
};

struct GraphBuild {
    bool ok{false};
    /// Why not, when not: the cycle, by name.
    std::string error;
    Graph graph;
    TopoResult order;
    PdcPlan pdc;
    ParamLayout params;
    /// Event track i belongs to this channel.
    std::vector<core::ChannelId> trackChannels;

    /// True when `other` would compile to the same render graph: the same nodes,
    /// wired the same way, with the same parameter slices and latencies. When it
    /// would, the compiled graph and its buffers are reused and only values change.
    [[nodiscard]] bool sameStructure(const GraphBuild& other) const noexcept;
};

[[nodiscard]] GraphBuild buildGraph(const project::Project& project, NodeStore& nodes);

} // namespace adx::graph
