// adx-thread: main
#include "engine/project/Rack.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>

#include "engine/dsp/Math.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/TypeCatalog.h"
#include "engine/project/Validate.h"

namespace adx::project {
namespace {

/// What a rack's parts became: everything a rack-relative path is resolved against.
struct Expanded {
    struct Layer {
        std::string name;
        core::ChannelId channel;
        core::InsertId insert;
        std::vector<core::SlotId> slots;
    };
    struct Send {
        std::string name;
        core::SendId send;
        core::InsertId target;
        std::vector<core::SlotId> slots;
    };
    core::InsertId insert;
    std::vector<core::SlotId> chain;
    std::vector<Layer> layers;
    std::vector<Send> sends;
};

std::vector<std::string_view> splitDots(std::string_view path) {
    std::vector<std::string_view> out;
    while (true) {
        const std::size_t dot = path.find('.');
        out.push_back(path.substr(0, dot));
        if (dot == std::string_view::npos) {
            return out;
        }
        path.remove_prefix(dot + 1);
    }
}

std::string joinFrom(const std::vector<std::string_view>& parts, std::size_t first) {
    std::string out;
    for (std::size_t i = first; i < parts.size(); ++i) {
        if (i > first) {
            out.push_back('.');
        }
        out += parts[i];
    }
    return out;
}

/// `text` as a slot number from 1 within `slots`; null when it is not one.
const core::SlotId* slotAt(const std::vector<core::SlotId>& slots, std::string_view text) {
    std::uint32_t n = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), n);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || n == 0 ||
        n > slots.size()) {
        return nullptr;
    }
    return &slots[n - 1];
}

/// The slot parameter `name` of slot `id`, added at its type's default when the rack's
/// SLOT line did not spell it: a path only resolves to a parameter the slot holds.
void ensureSlotParam(Project& project, core::SlotId id, std::string_view name, float fallback) {
    Slot* slot = project.mixer.findSlot(id);
    if (slot == nullptr || name == "mix" || name == "bypass" || slot->find(name) != nullptr) {
        return;
    }
    const TypeInfo* type = findEffectType(slot->type);
    const ParamDescriptor* descriptor = type != nullptr ? findParam(*type, name) : nullptr;
    slot->params.push_back(SlotParam{
        .name = std::string(name),
        .value = static_cast<double>(descriptor != nullptr ? descriptor->defaultValue : fallback)});
}

void ensureChannelParam(Project& project, core::ChannelId id, std::string_view name,
                        float fallback) {
    Channel* channel = project.find(id);
    if (channel == nullptr || name == "volume" || name == "pan" || name == "pitch" ||
        name == "arp.gate" || channel->instrument.find(name) != nullptr) {
        return;
    }
    const TypeInfo* type = findInstrumentType(channel->instrument.type);
    const ParamDescriptor* descriptor = type != nullptr ? findParam(*type, name) : nullptr;
    ParamValue value;
    value.name = std::string(name);
    value.value = static_cast<double>(descriptor != nullptr ? descriptor->defaultValue : fallback);
    channel->instrument.params.push_back(std::move(value));
}

std::string slotPath(const Project& project, core::SlotId slot, std::string_view leaf) {
    return "insert." + std::to_string(project.mixer.ownerOfSlot(slot).value) + ".slot." +
           std::to_string(slot.value) + "." + std::string(leaf);
}

/// A rack-relative path as a project path, creating the parameter it names where the
/// rack left it at its default. Empty when the path names nothing in the rack.
std::string projectPath(Project& project, const Expanded& rack, std::string_view path,
                        float fallback) {
    const std::vector<std::string_view> parts = splitDots(path);
    if (parts.size() < 3) {
        return {};
    }
    if (parts[0] == "layer") {
        const auto layer = std::ranges::find(rack.layers, parts[1], &Expanded::Layer::name);
        if (layer == rack.layers.end()) {
            return {};
        }
        if (parts[2] == "chain" && parts.size() >= 5) {
            const core::SlotId* slot = slotAt(layer->slots, parts[3]);
            if (slot == nullptr) {
                return {};
            }
            const std::string leaf = joinFrom(parts, 4);
            ensureSlotParam(project, *slot, leaf, fallback);
            return slotPath(project, *slot, leaf);
        }
        const Channel* channel = project.find(layer->channel);
        if (channel == nullptr) {
            return {};
        }
        const std::string leaf = joinFrom(parts, 2);
        ensureChannelParam(project, layer->channel, leaf, fallback);
        return "channel." + ParamRegistry::quoteSegment(channel->name) + "." + leaf;
    }
    if (parts[0] == "chain") {
        const core::SlotId* slot = slotAt(rack.chain, parts[1]);
        if (slot == nullptr) {
            return {};
        }
        const std::string leaf = joinFrom(parts, 2);
        ensureSlotParam(project, *slot, leaf, fallback);
        return slotPath(project, *slot, leaf);
    }
    if (parts[0] == "send") {
        const auto send = std::ranges::find(rack.sends, parts[1], &Expanded::Send::name);
        if (send == rack.sends.end()) {
            return {};
        }
        if (parts.size() == 3 && parts[2] == "level") {
            return "insert." + std::to_string(rack.insert.value) + ".send." +
                   std::to_string(send->send.value) + ".level";
        }
        const core::SlotId* slot = parts.size() >= 4 ? slotAt(send->slots, parts[2]) : nullptr;
        if (slot == nullptr) {
            return {};
        }
        const std::string leaf = joinFrom(parts, 3);
        ensureSlotParam(project, *slot, leaf, fallback);
        return slotPath(project, *slot, leaf);
    }
    return {};
}

core::SampleId sampleFor(Project& project, const std::string& path) {
    if (path.empty()) {
        return {};
    }
    if (const SampleRef* existing = project.resources.findByPath(path)) {
        return existing->id;
    }
    const core::SampleId id = project.newSampleId();
    project.resources.samples.push_back(SampleRef{.id = id, .path = path});
    return id;
}

/// Adds `slots` to `insert` and returns their ids, in order.
std::vector<core::SlotId> addSlots(Project& project, Insert& insert,
                                   const std::vector<RackSlot>& slots, core::InsertId rackInsert) {
    std::vector<core::SlotId> ids;
    for (const RackSlot& from : slots) {
        Slot slot;
        slot.id = project.newSlotId();
        slot.type = from.type;
        slot.mix = from.mix;
        slot.bypass = from.bypass;
        slot.params = from.params;
        slot.sidechain = from.keyRack ? rackInsert : core::InsertId{};
        slot.impulse = sampleFor(project, from.impulse);
        ids.push_back(slot.id);
        insert.slots.push_back(std::move(slot));
    }
    return ids;
}

bool nameTaken(const Project& project, std::string_view name) {
    const std::string prefix = std::string(name) + "/";
    const auto owns = [&](const std::string& other) {
        return other == name || other.starts_with(prefix);
    };
    return std::ranges::any_of(project.mixer.inserts,
                               [&](const Insert& i) { return owns(i.name); }) ||
           std::ranges::any_of(project.channels, [&](const Channel& c) { return owns(c.name); }) ||
           std::ranges::any_of(project.macros, [&](const Macro& m) { return owns(m.name); });
}

std::string uniqueName(const Project& project, std::string_view wanted) {
    const std::string base = sanitizeEntityName(wanted, "Rack");
    std::string name = base;
    for (int n = 2; nameTaken(project, name); ++n) {
        name = base + " " + std::to_string(n);
    }
    return name;
}

void addRoute(Project& project, core::InsertId from, core::InsertId to) {
    if (from.valid() && to.valid() && from != to) {
        project.mixer.routes.push_back(Route{.id = project.newRouteId(), .from = from, .to = to});
    }
}

RackSlot rackSlotOf(const Project& project, const Slot& slot, core::InsertId rackInsert) {
    RackSlot out;
    out.type = slot.type;
    out.mix = slot.mix;
    out.bypass = slot.bypass;
    out.params = slot.params;
    out.keyRack = slot.sidechain.valid() && slot.sidechain == rackInsert;
    if (const SampleRef* sample = project.resources.find(slot.impulse)) {
        out.impulse = sample->path;
    }
    return out;
}

std::string stripPrefix(const std::string& name, const std::string& rack) {
    const std::string prefix = rack + "/";
    return name.starts_with(prefix) ? name.substr(prefix.size()) : name;
}

/// A slot's place in a list of ids, from 1; 0 when it is not there.
std::size_t placeOf(const std::vector<core::SlotId>& slots, core::SlotId id) {
    const auto found = std::ranges::find(slots, id);
    return found == slots.end() ? 0 : static_cast<std::size_t>(found - slots.begin()) + 1;
}

/// The inverse of projectPath; empty when `ref` is outside the rack.
std::string rackPath(const Project& project, const Expanded& rack, ParamRef ref) {
    const auto layerOf = [&](core::ChannelId id) {
        return std::ranges::find(rack.layers, id, &Expanded::Layer::channel);
    };
    switch (ref.kind) {
    case ParamKind::ChannelVolume:
    case ParamKind::ChannelPan:
    case ParamKind::ChannelPitch:
    case ParamKind::ChannelArpGate:
    case ParamKind::ChannelInstrumentParam: {
        const auto layer = layerOf(core::ChannelId{ref.owner});
        const Channel* channel = project.find(core::ChannelId{ref.owner});
        if (layer == rack.layers.end() || channel == nullptr) {
            return {};
        }
        std::string leaf = "volume";
        if (ref.kind == ParamKind::ChannelPan) {
            leaf = "pan";
        } else if (ref.kind == ParamKind::ChannelPitch) {
            leaf = "pitch";
        } else if (ref.kind == ParamKind::ChannelArpGate) {
            leaf = "arp.gate";
        } else if (ref.kind == ParamKind::ChannelInstrumentParam) {
            if (ref.index >= channel->instrument.params.size()) {
                return {};
            }
            leaf = channel->instrument.params[ref.index].name;
        }
        return "layer." + layer->name + "." + leaf;
    }
    case ParamKind::SlotMix:
    case ParamKind::SlotBypass:
    case ParamKind::SlotParam: {
        const core::SlotId id{ref.owner};
        const Slot* slot = project.mixer.findSlot(id);
        if (slot == nullptr) {
            return {};
        }
        std::string leaf = ref.kind == ParamKind::SlotMix ? "mix" : "bypass";
        if (ref.kind == ParamKind::SlotParam) {
            if (ref.index >= slot->params.size()) {
                return {};
            }
            leaf = slot->params[ref.index].name;
        }
        if (const std::size_t n = placeOf(rack.chain, id); n != 0) {
            return "chain." + std::to_string(n) + "." + leaf;
        }
        for (const Expanded::Layer& layer : rack.layers) {
            if (const std::size_t n = placeOf(layer.slots, id); n != 0) {
                return "layer." + layer.name + ".chain." + std::to_string(n) + "." + leaf;
            }
        }
        for (const Expanded::Send& send : rack.sends) {
            if (const std::size_t n = placeOf(send.slots, id); n != 0) {
                return "send." + send.name + "." + std::to_string(n) + "." + leaf;
            }
        }
        return {};
    }
    case ParamKind::SendLevel: {
        const auto send =
            std::ranges::find(rack.sends, core::SendId{ref.owner}, &Expanded::Send::send);
        return send == rack.sends.end() ? std::string{} : "send." + send->name + ".level";
    }
    default:
        return {};
    }
}

std::vector<core::SlotId> slotIds(const Insert& insert) {
    std::vector<core::SlotId> ids;
    for (const Slot& slot : insert.slots) {
        ids.push_back(slot.id);
    }
    return ids;
}

} // namespace

InsertRack::InsertRack(Rack rack) : m_rack(std::move(rack)) {}

// NOLINTNEXTLINE(readability-function-size) - the expansion reads top to bottom.
void InsertRack::apply(Project& project) {
    m_marks = project.idMarks();
    m_channelCount = project.channels.size();
    m_insertCount = project.mixer.inserts.size();
    m_routeCount = project.mixer.routes.size();
    m_sampleCount = project.resources.samples.size();
    m_macroCount = project.macros.size();
    m_channels.clear();
    m_unresolved.clear();
    m_instance = uniqueName(project, m_rack.name);

    Expanded rack;
    // The rack's own insert: its chain, routed to the master.
    {
        Insert insert;
        insert.id = project.newInsertId();
        insert.name = m_instance;
        rack.insert = insert.id;
        rack.chain = addSlots(project, insert, m_rack.chain, insert.id);
        project.mixer.inserts.push_back(std::move(insert));
        addRoute(project, rack.insert, project.mixer.master);
    }
    m_insert = rack.insert;

    for (const RackSend& from : m_rack.sends) {
        Insert target;
        target.id = project.newInsertId();
        target.name = m_instance + "/" + sanitizeEntityName(from.name, "send");
        Expanded::Send send{.name = from.name,
                            .send = project.newSendId(),
                            .target = target.id,
                            .slots = addSlots(project, target, from.slots, rack.insert)};
        project.mixer.inserts.push_back(std::move(target));
        addRoute(project, send.target, project.mixer.master);
        if (Insert* own = project.mixer.find(rack.insert)) {
            own->sends.push_back(Send{.id = send.send,
                                      .target = send.target,
                                      .level = dsp::dbToGainF(from.levelDb),
                                      .preFader = false});
        }
        rack.sends.push_back(std::move(send));
    }

    for (const RackLayer& from : m_rack.layers) {
        Expanded::Layer layer;
        layer.name = from.name;
        const std::string name = m_instance + "/" + sanitizeEntityName(from.name, "layer");
        core::InsertId output = rack.insert;
        if (!from.chain.empty()) {
            Insert own;
            own.id = project.newInsertId();
            own.name = name;
            layer.insert = own.id;
            layer.slots = addSlots(project, own, from.chain, rack.insert);
            project.mixer.inserts.push_back(std::move(own));
            addRoute(project, layer.insert, rack.insert);
            output = layer.insert;
        }
        Channel channel;
        channel.id = project.newChannelId();
        channel.name = name;
        for (int n = 2; project.findChannelByName(channel.name) != nullptr; ++n) {
            channel.name = name + " " + std::to_string(n);
        }
        channel.instrument.type = from.type;
        channel.instrument.params = from.params;
        for (const RackZone& zone : from.zones) {
            SampleZone placed = zone.zone;
            placed.sample = sampleFor(project, zone.path);
            channel.instrument.zones.push_back(placed);
        }
        channel.output = output;
        channel.maxPolyphony = from.polyphony;
        channel.noteFilter = from.filter;
        channel.volume = dsp::dbToGainF(from.gainDb);
        channel.pan = from.pan;
        channel.pitchOffsetCents = from.pitchCents;
        layer.channel = channel.id;
        m_channels.push_back(channel.id);
        project.channels.push_back(std::move(channel));
        rack.layers.push_back(std::move(layer));
    }

    for (const RackMacro& from : m_rack.macros) {
        Macro macro;
        macro.id = project.newMacroId();
        macro.name = m_instance + "/" + sanitizeEntityName(from.name, "macro");
        for (int n = 2; project.findMacroByName(macro.name) != nullptr; ++n) {
            macro.name = m_instance + "/" + sanitizeEntityName(from.name, "macro") + " " +
                         std::to_string(n);
        }
        macro.value = std::clamp(from.value, 0.0F, 1.0F);
        for (const RackMacroTarget& target : from.targets) {
            MacroTarget placed{.path = projectPath(project, rack, target.path, target.minimum),
                               .target = {},
                               .minimum = target.minimum,
                               .maximum = target.maximum,
                               .curve = target.curve};
            const ParamResolution resolved = ParamRegistry::resolve(placed.path, project);
            if (resolved.ok()) {
                placed.target = resolved.ref;
            } else {
                m_unresolved.push_back(target.path);
                placed.path = target.path;
            }
            macro.targets.push_back(std::move(placed));
        }
        project.macros.push_back(std::move(macro));
    }
}

void InsertRack::revert(Project& project) {
    // apply() only appended: everything it made is at the back.
    project.macros.resize(m_macroCount);
    project.channels.resize(m_channelCount);
    project.mixer.inserts.resize(m_insertCount);
    project.mixer.routes.resize(m_routeCount);
    project.resources.samples.resize(m_sampleCount);
    project.restoreIdMarks(m_marks);
    m_insert = core::InsertId{};
    m_channels.clear();
}

std::string_view InsertRack::name() const noexcept {
    return "Insert rack";
}

CommandId InsertRack::kind() const noexcept {
    return CommandId::kInsertRack;
}

DirtyMask InsertRack::dirty() const noexcept {
    return dirty::kChannels | dirty::kMixer | dirty::kRouting | dirty::kResources;
}

// NOLINTNEXTLINE(readability-function-size) - the inverse of InsertRack::apply, in its order.
Rack rackFrom(const Project& project, core::InsertId insert) {
    Rack out;
    const Insert* own = project.mixer.find(insert);
    if (own == nullptr) {
        return out;
    }
    out.name = own->name;
    Expanded rack;
    rack.insert = insert;
    rack.chain = slotIds(*own);
    for (const Slot& slot : own->slots) {
        out.chain.push_back(rackSlotOf(project, slot, insert));
    }

    for (const Send& send : own->sends) {
        const Insert* target = project.mixer.find(send.target);
        if (target == nullptr) {
            continue;
        }
        RackSend saved;
        saved.name = stripPrefix(target->name, own->name);
        saved.levelDb = dsp::gainToDbF(send.level, -120.0F);
        for (const Slot& slot : target->slots) {
            saved.slots.push_back(rackSlotOf(project, slot, insert));
        }
        rack.sends.push_back(Expanded::Send{
            .name = saved.name, .send = send.id, .target = target->id, .slots = slotIds(*target)});
        out.sends.push_back(std::move(saved));
    }

    const auto feedsRack = [&](core::InsertId from) {
        return std::ranges::any_of(project.mixer.routes, [&](const Route& route) {
            return route.from == from && route.to == insert;
        });
    };
    for (const Channel& channel : project.channels) {
        const bool direct = channel.output == insert;
        const Insert* layerInsert = direct ? nullptr : project.mixer.find(channel.output);
        if (!direct && (layerInsert == nullptr || channel.output == project.mixer.master ||
                        !feedsRack(channel.output))) {
            continue;
        }
        RackLayer layer;
        layer.name = stripPrefix(channel.name, own->name);
        layer.type = channel.instrument.type;
        layer.params = channel.instrument.params;
        for (const SampleZone& zone : channel.instrument.zones) {
            const SampleRef* sample = project.resources.find(zone.sample);
            SampleZone bare = zone;
            bare.sample = core::SampleId{};
            layer.zones.push_back(
                RackZone{.zone = bare, .path = sample != nullptr ? sample->path : std::string{}});
        }
        layer.filter = channel.noteFilter;
        layer.gainDb = dsp::gainToDbF(channel.volume, -120.0F);
        layer.pan = channel.pan;
        layer.pitchCents = channel.pitchOffsetCents;
        layer.polyphony = channel.maxPolyphony;
        Expanded::Layer expanded{
            .name = layer.name, .channel = channel.id, .insert = {}, .slots = {}};
        if (layerInsert != nullptr) {
            expanded.insert = layerInsert->id;
            expanded.slots = slotIds(*layerInsert);
            for (const Slot& slot : layerInsert->slots) {
                layer.chain.push_back(rackSlotOf(project, slot, insert));
            }
        }
        rack.layers.push_back(std::move(expanded));
        out.layers.push_back(std::move(layer));
    }

    for (const Macro& macro : project.macros) {
        RackMacro saved;
        saved.name = stripPrefix(macro.name, own->name);
        saved.value = macro.value;
        bool inside = !macro.targets.empty();
        for (const MacroTarget& target : macro.targets) {
            std::string path = rackPath(project, rack, target.target);
            if (path.empty()) {
                inside = false;
                break;
            }
            saved.targets.push_back(RackMacroTarget{.path = std::move(path),
                                                    .minimum = target.minimum,
                                                    .maximum = target.maximum,
                                                    .curve = target.curve});
        }
        if (inside) {
            out.macros.push_back(std::move(saved));
        }
    }
    return out;
}

} // namespace adx::project
