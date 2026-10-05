// adx-thread: main
#include "engine/graph/GraphBuilder.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "engine/effects/Factory.h"
#include "engine/graph/nodes/ChannelNode.h"
#include "engine/graph/nodes/InsertNode.h"
#include "engine/graph/nodes/SendNode.h"
#include "engine/graph/nodes/SlotNode.h"
#include "engine/instruments/Factory.h"
#include "engine/project/Project.h"
#include "engine/project/TypeCatalog.h"

namespace adx::graph {

// --- NodeStore ---------------------------------------------------------------------

void NodeStore::setPrepareInfo(PrepareInfo info) {
    if (info.sampleRate != m_info.sampleRate || info.maxBlockFrames != m_info.maxBlockFrames) {
        m_nodes.clear();
    }
    m_info = info;
}

template<class Make> std::shared_ptr<Node> NodeStore::getOrMake(Key key, Make make) {
    Entry& entry = m_nodes[key];
    if (!entry.node) {
        entry.node = make();
        entry.node->prepare(m_info);
        ++m_created;
    }
    entry.used = true;
    return entry.node;
}

std::shared_ptr<Node> NodeStore::channel(const project::Channel& channel,
                                         const project::Resources& resources,
                                         const project::Project* project) {
    const Key key{Kind::Channel, channel.id.value};
    // A channel node is reusable only while the things its voice pool was sized and
    // configured from are unchanged. Anything else - a new polyphony, a new steal
    // mode - is a new pool, and a new pool is a new node. The instrument type is part
    // of it (P3-2), and so is an instrument's structure: a Sampler's zones and the
    // files they resolve to (instruments::configMatches).
    const instruments::InstrumentContext context{
        .resources = &resources, .pool = m_pool, .project = project};
    const auto found = m_nodes.find(key);
    if (found != m_nodes.end() && found->second.node) {
        const auto* existing = static_cast<const ChannelNode*>(found->second.node.get());
        if (!instruments::configMatches(*existing, channel, context)) {
            m_nodes.erase(found);
        }
    }
    return getOrMake(
        key, [&channel, &context] { return instruments::makeInstrument(channel, context); });
}

std::shared_ptr<Node> NodeStore::fader(core::InsertId id) {
    return getOrMake(Key{Kind::Fader, id.value}, [] { return std::make_shared<InsertNode>(); });
}

std::shared_ptr<Node> NodeStore::meter(core::InsertId id, bool master) {
    const Key key{Kind::Meter, id.value};
    // Whether a meter measures true peak is fixed at prepare: the insert becoming or
    // ceasing to be the master is a new meter.
    const auto found = m_nodes.find(key);
    if (found != m_nodes.end() && found->second.node &&
        static_cast<const MeterNode*>(found->second.node.get())->truePeak() != master) {
        m_nodes.erase(found);
    }
    return getOrMake(key, [master] {
        auto node = std::make_shared<MeterNode>();
        node->setTruePeak(master);
        return node;
    });
}

void NodeStore::forEachMeter(
    const std::function<void(core::InsertId, const MeterNode&)>& visit) const {
    for (const auto& [key, entry] : m_nodes) {
        if (key.first == Kind::Meter && entry.node) {
            visit(core::InsertId{key.second}, static_cast<const MeterNode&>(*entry.node));
        }
    }
}

std::shared_ptr<Node> NodeStore::slot(const project::Slot& slot,
                                      const project::Resources& resources) {
    const Key key{Kind::Slot, slot.id.value};
    // A slot whose effect type changed is a new node: the old effect's state means
    // nothing to the new one.
    const auto found = m_nodes.find(key);
    if (found != m_nodes.end() && found->second.node) {
        const auto* existing = static_cast<const SlotNode*>(found->second.node.get());
        if (!effects::configMatches(*existing, slot, &resources)) {
            m_nodes.erase(found);
        }
    }
    return getOrMake(key, [&slot, &resources] { return effects::makeEffect(slot, &resources); });
}

std::shared_ptr<Node> NodeStore::send(core::SendId id) {
    return getOrMake(Key{Kind::Send, id.value}, [] { return std::make_shared<SendNode>(); });
}

std::shared_ptr<const MeterNode> NodeStore::findMeter(core::InsertId id) const {
    const auto found = m_nodes.find(Key{Kind::Meter, id.value});
    if (found == m_nodes.end()) {
        return nullptr;
    }
    return std::static_pointer_cast<const MeterNode>(found->second.node);
}

void NodeStore::pruneUnused() {
    std::erase_if(m_nodes, [](const auto& item) { return !item.second.used; });
    for (auto& item : m_nodes) {
        item.second.used = false;
    }
}

// --- parameters --------------------------------------------------------------------

namespace {

/// Everything fillParams looks up by id, indexed once per call.
struct Lookup {
    std::unordered_map<std::uint32_t, const project::Channel*> channels;
    std::unordered_map<std::uint32_t, const project::Insert*> inserts;
    std::unordered_map<std::uint32_t, const project::Slot*> slots;
    std::unordered_map<std::uint32_t, const project::Send*> sends;
    std::unordered_set<std::uint32_t> audibleInserts;
    bool anyChannelSolo{false};
};

/// Which inserts a solo leaves audible: the soloed ones, everything they feed (or the
/// soloed signal could not reach the speakers), everything that feeds them (or the
/// soloed strip would be silent), and the master.
std::unordered_set<std::uint32_t> audibleUnderSolo(const project::Project& project) {
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> down;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> up;
    const auto link = [&](core::InsertId from, core::InsertId to) {
        down[from.value].push_back(to.value);
        up[to.value].push_back(from.value);
    };
    for (const project::Route& route : project.mixer.routes) {
        link(route.from, route.to);
    }
    for (const project::Insert& insert : project.mixer.inserts) {
        for (const project::Send& send : insert.sends) {
            link(insert.id, send.target);
        }
    }

    std::unordered_set<std::uint32_t> audible;
    std::vector<std::uint32_t> pending;
    bool anySolo = false;
    for (const project::Insert& insert : project.mixer.inserts) {
        if (insert.soloed) {
            anySolo = true;
            pending.push_back(insert.id.value);
        }
    }
    if (!anySolo) {
        for (const project::Insert& insert : project.mixer.inserts) {
            audible.insert(insert.id.value);
        }
        return audible;
    }
    audible.insert(project.mixer.master.value);
    for (auto* direction : {&down, &up}) {
        std::vector<std::uint32_t> stack = pending;
        std::unordered_set<std::uint32_t> seen;
        while (!stack.empty()) {
            const std::uint32_t at = stack.back();
            stack.pop_back();
            if (!seen.insert(at).second) {
                continue;
            }
            audible.insert(at);
            for (const std::uint32_t next : (*direction)[at]) {
                stack.push_back(next);
            }
        }
    }
    return audible;
}

/// Fills `lookup` in place. An out-parameter rather than a return value: MSVC's
/// debug unordered_map can allocate when moved, so returning one by value would give
/// the struct a move constructor that is noexcept by declaration and throwing in fact.
void indexProject(const project::Project& project, Lookup& lookup) {
    for (const project::Channel& channel : project.channels) {
        lookup.channels[channel.id.value] = &channel;
        lookup.anyChannelSolo = lookup.anyChannelSolo || channel.soloed;
    }
    for (const project::Insert& insert : project.mixer.inserts) {
        lookup.inserts[insert.id.value] = &insert;
        for (const project::Slot& slot : insert.slots) {
            lookup.slots[slot.id.value] = &slot;
        }
        for (const project::Send& send : insert.sends) {
            lookup.sends[send.id.value] = &send;
        }
    }
    lookup.audibleInserts = audibleUnderSolo(project);
}

template<class Map> auto* findIn(const Map& map, std::uint32_t id) {
    const auto found = map.find(id);
    return found == map.end() ? nullptr : found->second;
}

/// A named parameter's value, or the component of its curve a binding asks for. A
/// parameter written without `curve=` has the linear curve's components.
float valueOrCurvePart(const project::ParamValue& value, const ParamBinding& binding) {
    using project::CurvePart;
    if (binding.part == CurvePart::None) {
        return static_cast<float>(value.value);
    }
    if (!value.hasCurve) {
        return binding.fallback;
    }
    const core::Curve& curve = value.curve;
    switch (binding.part) {
    case CurvePart::Kind:
        return static_cast<float>(curve.kind);
    case CurvePart::Tension:
        return curve.tension;
    case CurvePart::C1x:
        return curve.c1x;
    case CurvePart::C1y:
        return curve.c1y;
    case CurvePart::C2x:
        return curve.c2x;
    case CurvePart::C2y:
        return curve.c2y;
    case CurvePart::None:
        break;
    }
    return binding.fallback;
}

float resolve(const ParamBinding& binding, const Lookup& lookup) {
    switch (binding.source) {
    case ParamSource::ChannelVolume:
    case ParamSource::ChannelPan:
    case ParamSource::ChannelAudible:
    case ParamSource::ChannelPitch:
    case ParamSource::ChannelInstrument: {
        const project::Channel* channel = findIn(lookup.channels, binding.owner);
        if (channel == nullptr) {
            return 0.0F;
        }
        switch (binding.source) {
        case ParamSource::ChannelVolume:
            return channel->volume;
        case ParamSource::ChannelPan:
            return channel->pan;
        case ParamSource::ChannelAudible:
            return !channel->muted && (!lookup.anyChannelSolo || channel->soloed) ? 1.0F : 0.0F;
        case ParamSource::ChannelPitch:
            return channel->pitchOffsetCents;
        default:
            if (binding.index >= channel->instrument.params.size()) {
                return binding.fallback;
            }
            return valueOrCurvePart(channel->instrument.params[binding.index], binding);
        }
    }
    case ParamSource::InsertGain:
    case ParamSource::InsertPan:
    case ParamSource::InsertWidth:
    case ParamSource::InsertAudible:
    case ParamSource::InsertPolarity: {
        const project::Insert* insert = findIn(lookup.inserts, binding.owner);
        if (insert == nullptr) {
            return 0.0F;
        }
        switch (binding.source) {
        case ParamSource::InsertGain:
            return insert->gain;
        case ParamSource::InsertPan:
            return insert->pan;
        case ParamSource::InsertWidth:
            return insert->stereoSeparation;
        case ParamSource::InsertAudible:
            return !insert->muted && lookup.audibleInserts.contains(insert->id.value) ? 1.0F : 0.0F;
        default:
            return insert->polarityInvert ? -1.0F : 1.0F;
        }
    }
    case ParamSource::SlotMix:
    case ParamSource::SlotBypass:
    case ParamSource::SlotEffect: {
        const project::Slot* slot = findIn(lookup.slots, binding.owner);
        if (slot == nullptr) {
            return 0.0F;
        }
        if (binding.source == ParamSource::SlotMix) {
            return slot->mix;
        }
        if (binding.source == ParamSource::SlotBypass) {
            return slot->bypass ? 1.0F : 0.0F;
        }
        return binding.index < slot->params.size()
                   ? static_cast<float>(slot->params[binding.index].value)
                   : binding.fallback;
    }
    case ParamSource::SendLevel: {
        const project::Send* send = findIn(lookup.sends, binding.owner);
        return send == nullptr ? 0.0F : send->level;
    }
    }
    return 0.0F;
}

/// The ParamRef a binding answers to, or an invalid ref for the derived ones.
project::ParamRef refOf(const ParamBinding& binding) {
    using project::ParamKind;
    if (binding.index == kAbsentParam || binding.part != project::CurvePart::None) {
        return project::ParamRef{};
    }
    const auto make = [&binding](ParamKind kind) {
        return project::ParamRef{.owner = binding.owner, .index = binding.index, .kind = kind};
    };
    switch (binding.source) {
    case ParamSource::ChannelVolume:
        return make(ParamKind::ChannelVolume);
    case ParamSource::ChannelPan:
        return make(ParamKind::ChannelPan);
    case ParamSource::ChannelPitch:
        return make(ParamKind::ChannelPitch);
    case ParamSource::ChannelInstrument:
        return make(ParamKind::ChannelInstrumentParam);
    case ParamSource::InsertGain:
        return make(ParamKind::InsertGain);
    case ParamSource::InsertPan:
        return make(ParamKind::InsertPan);
    case ParamSource::InsertWidth:
        return make(ParamKind::InsertWidth);
    case ParamSource::SlotMix:
        return make(ParamKind::SlotMix);
    case ParamSource::SlotBypass:
        return make(ParamKind::SlotBypass);
    case ParamSource::SlotEffect:
        return make(ParamKind::SlotParam);
    case ParamSource::SendLevel:
        return make(ParamKind::SendLevel);
    case ParamSource::ChannelAudible:
    case ParamSource::InsertAudible:
    case ParamSource::InsertPolarity:
        return project::ParamRef{};
    }
    return project::ParamRef{};
}

/// Appends one binding per descriptor of `type`. `indexOf` maps a parameter name to
/// its position in the model's list, or kAbsentParam.
template<class IndexOf>
void appendTyped(ParamLayout& layout, const project::TypeInfo& type, ParamSource source,
                 std::uint32_t owner, IndexOf indexOf) {
    for (const project::ParamDescriptor& descriptor : type.params) {
        layout.bindings.push_back(ParamBinding{.source = source,
                                               .owner = owner,
                                               .index = indexOf(descriptor.name),
                                               .part = descriptor.curve,
                                               .fallback = descriptor.defaultValue});
    }
}

/// Appends a node's parameter slice and returns where it starts.
std::uint32_t appendParams(ParamLayout& layout, std::initializer_list<ParamBinding> bindings) {
    const auto base = static_cast<std::uint32_t>(layout.bindings.size());
    layout.bindings.insert(layout.bindings.end(), bindings.begin(), bindings.end());
    return base;
}

} // namespace

void fillParams(const project::Project& project, const ParamLayout& layout,
                std::vector<float>& out) {
    Lookup lookup;
    indexProject(project, lookup);
    out.resize(layout.bindings.size());
    for (std::size_t i = 0; i < layout.bindings.size(); ++i) {
        out[i] = resolve(layout.bindings[i], lookup);
    }
}

// --- the graph ---------------------------------------------------------------------

namespace {

std::string insertLabel(core::InsertId id) {
    return "insert." + std::to_string(id.value);
}

/// Adds one insert's chain - slots, fader, meter - and returns (entry, fader). Slots
/// keyed from another insert are recorded in `keyed`, to be wired once every fader
/// exists. The master's meter measures true peak as well.
std::pair<NodeId, NodeId> addInsertChain(const project::Insert& insert, bool master, Graph& graph,
                                         ParamLayout& layout, NodeStore& nodes,
                                         std::vector<std::pair<NodeId, core::InsertId>>& keyed,
                                         const project::Resources& resources) {
    const std::string label = insertLabel(insert.id);

    std::vector<const project::Slot*> slots;
    slots.reserve(insert.slots.size());
    for (const project::Slot& slot : insert.slots) {
        slots.push_back(&slot);
    }
    // Slots process in ascending id order (Mixer.h), whatever order the vector holds.
    std::ranges::sort(slots, {}, [](const project::Slot* slot) { return slot->id.value; });

    NodeId entry{kNone};
    NodeId previous{kNone};
    for (const project::Slot* slot : slots) {
        const NodeId id = graph.add(nodes.slot(*slot, resources), label);
        if (slot->sidechain.valid()) {
            keyed.emplace_back(id, slot->sidechain);
        }
        GraphNode& node = *graph.find(id);
        node.paramBase = appendParams(
            layout, {ParamBinding{.source = ParamSource::SlotMix, .owner = slot->id.value},
                     ParamBinding{.source = ParamSource::SlotBypass, .owner = slot->id.value}});
        // The effect's parameters in the order its descriptor table names them - the
        // order its node reads them - whatever order the file listed them in.
        if (const project::TypeInfo* type = project::findEffectType(slot->type)) {
            appendTyped(layout, *type, ParamSource::SlotEffect, slot->id.value,
                        [slot](std::string_view name) {
                            const auto match =
                                std::ranges::find(slot->params, name, &project::SlotParam::name);
                            return match == slot->params.end()
                                       ? kAbsentParam
                                       : static_cast<std::uint16_t>(match - slot->params.begin());
                        });
        }
        node.paramCount = static_cast<std::uint32_t>(layout.bindings.size()) - node.paramBase;
        if (previous.v == kNone) {
            entry = id;
        } else {
            graph.connect(previous, kPortPost, id, kPortMain);
        }
        previous = id;
    }

    const NodeId fader = graph.add(nodes.fader(insert.id), label);
    {
        GraphNode& node = *graph.find(fader);
        const std::uint32_t owner = insert.id.value;
        node.paramBase = appendParams(
            layout, {ParamBinding{.source = ParamSource::InsertGain, .owner = owner},
                     ParamBinding{.source = ParamSource::InsertPan, .owner = owner},
                     ParamBinding{.source = ParamSource::InsertWidth, .owner = owner},
                     ParamBinding{.source = ParamSource::InsertAudible, .owner = owner},
                     ParamBinding{.source = ParamSource::InsertPolarity, .owner = owner}});
        node.paramCount = kInsertParamCount;
    }
    if (previous.v == kNone) {
        entry = fader;
    } else {
        graph.connect(previous, kPortPost, fader, kPortMain);
    }

    const NodeId meter = graph.add(nodes.meter(insert.id, master), label);
    graph.connect(fader, kPortPost, meter, kPortMain);
    return {entry, fader};
}

} // namespace

// NOLINTNEXTLINE(readability-function-size)
GraphBuild buildGraph(const project::Project& project, NodeStore& nodes) {
    GraphBuild build;
    Graph& graph = build.graph;
    ParamLayout& layout = build.params;

    std::unordered_map<std::uint32_t, NodeId> entries;
    std::unordered_map<std::uint32_t, NodeId> faders;

    // Channels are grouped with the insert they feed, so a channel's id sits just
    // before its strip's: the sort's ascending-id tie-break then runs each channel
    // straight into its strip, which keeps the number of live accumulators - and so
    // of buffers - small.
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> channelsByInsert;
    std::vector<std::size_t> orphans;
    for (std::size_t c = 0; c < project.channels.size(); ++c) {
        const core::InsertId output = project.channels[c].output;
        if (project.mixer.find(output) != nullptr) {
            channelsByInsert[output.value].push_back(c);
        } else {
            orphans.push_back(c);
        }
    }

    const auto addChannel = [&](std::size_t ordinal) {
        const project::Channel& channel = project.channels[ordinal];
        const NodeId id = graph.add(nodes.channel(channel, project.resources, &project),
                                    "channel." + channel.name);
        GraphNode& node = *graph.find(id);
        node.eventTrack = static_cast<std::uint32_t>(ordinal);
        const std::uint32_t owner = channel.id.value;
        node.paramBase = appendParams(
            layout, {ParamBinding{.source = ParamSource::ChannelVolume, .owner = owner},
                     ParamBinding{.source = ParamSource::ChannelPan, .owner = owner},
                     ParamBinding{.source = ParamSource::ChannelAudible, .owner = owner},
                     ParamBinding{.source = ParamSource::ChannelPitch, .owner = owner}});
        if (const project::TypeInfo* type = project::findInstrumentType(channel.instrument.type)) {
            const std::vector<project::ParamValue>& values = channel.instrument.params;
            appendTyped(layout, *type, ParamSource::ChannelInstrument, owner,
                        [&values](std::string_view name) {
                            const auto match =
                                std::ranges::find(values, name, &project::ParamValue::name);
                            return match == values.end()
                                       ? kAbsentParam
                                       : static_cast<std::uint16_t>(match - values.begin());
                        });
        }
        node.paramCount = static_cast<std::uint32_t>(layout.bindings.size()) - node.paramBase;
        return id;
    };

    std::vector<std::pair<NodeId, core::InsertId>> pendingOutputs;
    std::vector<std::pair<NodeId, core::InsertId>> keyed;
    for (const project::Insert& insert : project.mixer.inserts) {
        for (const std::size_t ordinal : channelsByInsert[insert.id.value]) {
            pendingOutputs.emplace_back(addChannel(ordinal), insert.id);
        }
        const auto [entry, fader] = addInsertChain(insert, insert.id == project.mixer.master, graph,
                                                   layout, nodes, keyed, project.resources);
        entries[insert.id.value] = entry;
        faders[insert.id.value] = fader;
        for (const project::Send& send : insert.sends) {
            const NodeId id = graph.add(nodes.send(send.id), insertLabel(insert.id));
            GraphNode& node = *graph.find(id);
            node.paramBase = appendParams(
                layout, {ParamBinding{.source = ParamSource::SendLevel, .owner = send.id.value}});
            node.paramCount = kSendParamCount;
            graph.connect(fader, send.preFader ? kPortPre : kPortPost, id, kPortMain);
            if (project.mixer.find(send.target) != nullptr) {
                // Connected once every insert exists; recorded now to keep the order.
                pendingOutputs.emplace_back(id, send.target);
            }
        }
    }
    // A channel whose output insert does not exist still gets a node - it keeps its
    // event track and its voices - but has nowhere to go. Validate reports it.
    for (const std::size_t ordinal : orphans) {
        static_cast<void>(addChannel(ordinal));
    }

    for (const auto& [from, target] : pendingOutputs) {
        graph.connect(from, kPortPost, entries[target.value], kPortMain);
    }
    // Sidechains: the key insert's post-fader signal into the slot's key port. Only
    // into a node that has one - an effect type without a key input ignores the
    // setting rather than growing a port.
    for (const auto& [slotNode, key] : keyed) {
        const GraphNode* node = graph.find(slotNode);
        if (faders.contains(key.value) && node != nullptr &&
            node->node->ports().inputs > kPortSidechain) {
            graph.connect(faders[key.value], kPortPost, slotNode, kPortSidechain);
        }
    }
    for (const project::Route& route : project.mixer.routes) {
        if (faders.contains(route.from.value) && entries.contains(route.to.value)) {
            graph.connect(faders[route.from.value], kPortPost, entries[route.to.value], kPortMain);
        }
    }
    if (faders.contains(project.mixer.master.value)) {
        graph.setOutput(faders[project.mixer.master.value]);
    }

    build.trackChannels.reserve(project.channels.size());
    for (const project::Channel& channel : project.channels) {
        build.trackChannels.push_back(channel.id);
    }

    for (const ParamBinding& binding : layout.bindings) {
        const project::ParamRef ref = refOf(binding);
        if (ref.valid()) {
            layout.index.push_back(project::ParamSlot{
                .ref = ref,
                .index = static_cast<std::uint32_t>(&binding - layout.bindings.data())});
        }
    }
    std::ranges::sort(layout.index, [](const project::ParamSlot& a, const project::ParamSlot& b) {
        return project::paramRefLess(a.ref, b.ref);
    });

    build.order = topoSort(graph);
    if (!build.order.ok) {
        build.error = build.order.message;
        return build;
    }
    build.pdc = computePdc(graph, build.order.order);
    build.ok = true;
    return build;
}

bool GraphBuild::sameStructure(const GraphBuild& other) const noexcept {
    if (ok != other.ok || graph.nodes().size() != other.graph.nodes().size() ||
        graph.edges() != other.graph.edges() || graph.hasOutput() != other.graph.hasOutput() ||
        graph.output() != other.graph.output() || pdc.latency != other.pdc.latency ||
        order.order != other.order.order ||
        params.bindings.size() != other.params.bindings.size()) {
        return false;
    }
    for (std::size_t i = 0; i < graph.nodes().size(); ++i) {
        const GraphNode& a = graph.nodes()[i];
        const GraphNode& b = other.graph.nodes()[i];
        if (a.node != b.node || a.eventTrack != b.eventTrack || a.timeSource != b.timeSource ||
            a.paramBase != b.paramBase || a.paramCount != b.paramCount) {
            return false;
        }
    }
    for (std::size_t i = 0; i < params.bindings.size(); ++i) {
        const ParamBinding& a = params.bindings[i];
        const ParamBinding& b = other.params.bindings[i];
        if (a.source != b.source || a.owner != b.owner || a.index != b.index || a.part != b.part ||
            a.fallback != b.fallback) {
            return false;
        }
    }
    return true;
}

} // namespace adx::graph
