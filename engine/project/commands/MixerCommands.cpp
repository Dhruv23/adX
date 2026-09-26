#include "engine/project/commands/MixerCommands.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace adx::project {
namespace {

[[nodiscard]] double readField(const Insert& insert, InsertField field) noexcept {
    switch (field) {
    case InsertField::Gain:
        return insert.gain;
    case InsertField::Pan:
        return insert.pan;
    case InsertField::StereoSeparation:
        return insert.stereoSeparation;
    case InsertField::Muted:
        return insert.muted ? 1.0 : 0.0;
    case InsertField::Soloed:
        return insert.soloed ? 1.0 : 0.0;
    case InsertField::PolarityInvert:
        return insert.polarityInvert ? 1.0 : 0.0;
    }
    return 0.0;
}

void writeField(Insert& insert, InsertField field, double value) noexcept {
    switch (field) {
    case InsertField::Gain:
        insert.gain = static_cast<float>(value);
        break;
    case InsertField::Pan:
        insert.pan = static_cast<float>(value);
        break;
    case InsertField::StereoSeparation:
        insert.stereoSeparation = static_cast<float>(value);
        break;
    case InsertField::Muted:
        insert.muted = value != 0.0;
        break;
    case InsertField::Soloed:
        insert.soloed = value != 0.0;
        break;
    case InsertField::PolarityInvert:
        insert.polarityInvert = value != 0.0;
        break;
    }
}

} // namespace

// --- AddInsert ---------------------------------------------------------------

AddInsert::AddInsert(Insert prototype) : m_prototype(std::move(prototype)) {}

void AddInsert::apply(Project& project) {
    m_marks = project.idMarks();
    m_id = project.newInsertId(m_prototype.id);
    Insert insert = m_prototype;
    insert.id = m_id;
    for (Slot& slot : insert.slots) {
        slot.id = project.newSlotId(slot.id);
    }
    for (Send& send : insert.sends) {
        send.id = project.newSendId(send.id);
    }
    project.mixer.inserts.push_back(std::move(insert));
    // The first insert a project gets is its master, so a project built entirely by
    // commands never needs a separate "and now set the master" step it could forget.
    if (!project.mixer.master.valid()) {
        project.mixer.master = m_id;
    }
}

void AddInsert::revert(Project& project) {
    std::erase_if(project.mixer.inserts, [this](const Insert& i) { return i.id == m_id; });
    if (project.mixer.master == m_id) {
        project.mixer.master = core::InsertId{};
    }
    project.restoreIdMarks(m_marks);
    m_id = core::InsertId{};
}

std::string_view AddInsert::name() const noexcept {
    return "Add insert";
}

CommandId AddInsert::kind() const noexcept {
    return CommandId::kAddInsert;
}

DirtyMask AddInsert::dirty() const noexcept {
    return dirty::kMixer;
}

// --- RemoveInsert ------------------------------------------------------------

RemoveInsert::RemoveInsert(core::InsertId id) : m_id(id) {}

void RemoveInsert::apply(Project& project) {
    auto& inserts = project.mixer.inserts;
    const auto match = std::ranges::find(inserts, m_id, &Insert::id);
    m_removed = match != inserts.end();
    if (!m_removed) {
        return;
    }
    m_index = static_cast<std::size_t>(std::distance(inserts.begin(), match));
    m_previous = *match;
    inserts.erase(match);

    m_previousMaster = project.mixer.master;
    if (project.mixer.master == m_id) {
        project.mixer.master = core::InsertId{};
    }

    m_routes.clear();
    auto& routes = project.mixer.routes;
    for (std::size_t i = routes.size(); i-- > 0;) {
        if (routes[i].from != m_id && routes[i].to != m_id) {
            continue;
        }
        m_routes.emplace_back(i, routes[i]);
        routes.erase(routes.begin() + static_cast<std::ptrdiff_t>(i));
    }

    m_sends.clear();
    for (Insert& insert : inserts) {
        for (std::size_t i = insert.sends.size(); i-- > 0;) {
            if (insert.sends[i].target != m_id) {
                continue;
            }
            m_sends.push_back(
                DetachedSend{.owner = insert.id, .index = i, .send = insert.sends[i]});
            insert.sends.erase(insert.sends.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    m_channels.clear();
    for (Channel& channel : project.channels) {
        if (channel.output != m_id) {
            continue;
        }
        m_channels.push_back(Rerouted{.channel = channel.id, .previous = channel.output});
        channel.output = project.mixer.master;
    }
}

void RemoveInsert::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    auto& inserts = project.mixer.inserts;
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, inserts.size()));
    inserts.insert(inserts.begin() + at, m_previous);
    project.mixer.master = m_previousMaster;

    auto& routes = project.mixer.routes;
    for (const auto& [index, route] : std::views::reverse(m_routes)) {
        const auto position = static_cast<std::ptrdiff_t>(std::min(index, routes.size()));
        routes.insert(routes.begin() + position, route);
    }
    for (const DetachedSend& detached : std::views::reverse(m_sends)) {
        Insert* owner = project.mixer.find(detached.owner);
        if (owner == nullptr) {
            continue;
        }
        const auto index =
            static_cast<std::ptrdiff_t>(std::min(detached.index, owner->sends.size()));
        owner->sends.insert(owner->sends.begin() + index, detached.send);
    }
    for (const Rerouted& rerouted : m_channels) {
        if (Channel* channel = project.find(rerouted.channel)) {
            channel->output = rerouted.previous;
        }
    }
}

std::string_view RemoveInsert::name() const noexcept {
    return "Remove insert";
}

CommandId RemoveInsert::kind() const noexcept {
    return CommandId::kRemoveInsert;
}

DirtyMask RemoveInsert::dirty() const noexcept {
    return dirty::kMixer | dirty::kRouting | dirty::kChannels;
}

// --- RenameInsert ------------------------------------------------------------

RenameInsert::RenameInsert(core::InsertId id, std::string insertName)
    : m_id(id), m_name(std::move(insertName)) {}

void RenameInsert::apply(Project& project) {
    Insert* insert = project.mixer.find(m_id);
    if (insert == nullptr) {
        return;
    }
    m_previous = insert->name;
    insert->name = m_name;
}

void RenameInsert::revert(Project& project) {
    if (Insert* insert = project.mixer.find(m_id)) {
        insert->name = m_previous;
    }
}

std::string_view RenameInsert::name() const noexcept {
    return "Rename insert";
}

CommandId RenameInsert::kind() const noexcept {
    return CommandId::kRenameInsert;
}

DirtyMask RenameInsert::dirty() const noexcept {
    return dirty::kMixer;
}

std::uint64_t RenameInsert::targetKey() const noexcept {
    return m_id.value;
}

bool RenameInsert::coalesceWith(const Command& next) {
    m_name = static_cast<const RenameInsert&>(next).m_name;
    return true;
}

// --- SetInsertValue ----------------------------------------------------------

SetInsertValue::SetInsertValue(core::InsertId id, InsertField field, double value)
    : m_id(id), m_field(field), m_value(value) {}

void SetInsertValue::apply(Project& project) {
    Insert* insert = project.mixer.find(m_id);
    if (insert == nullptr) {
        return;
    }
    m_previous = readField(*insert, m_field);
    writeField(*insert, m_field, m_value);
}

void SetInsertValue::revert(Project& project) {
    if (Insert* insert = project.mixer.find(m_id)) {
        writeField(*insert, m_field, m_previous);
    }
}

std::string_view SetInsertValue::name() const noexcept {
    return "Set mixer value";
}

CommandId SetInsertValue::kind() const noexcept {
    return CommandId::kSetInsertValue;
}

DirtyMask SetInsertValue::dirty() const noexcept {
    return dirty::kMixer;
}

std::uint64_t SetInsertValue::targetKey() const noexcept {
    return (static_cast<std::uint64_t>(m_field) << 32U) | m_id.value;
}

bool SetInsertValue::coalesceWith(const Command& next) {
    m_value = static_cast<const SetInsertValue&>(next).m_value;
    return true;
}

// --- AddSlot -----------------------------------------------------------------

AddSlot::AddSlot(core::InsertId insert, Slot prototype)
    : m_insert(insert), m_prototype(std::move(prototype)) {}

void AddSlot::apply(Project& project) {
    Insert* insert = project.mixer.find(m_insert);
    if (insert == nullptr) {
        return;
    }
    m_marks = project.idMarks();
    m_id = project.newSlotId(m_prototype.id);
    Slot slot = m_prototype;
    slot.id = m_id;
    insert->slots.push_back(std::move(slot));
}

void AddSlot::revert(Project& project) {
    // apply() returns without allocating when the insert is gone, and m_marks is
    // still zero in that case. Restoring it would reset every id counter in the
    // project to zero, and the next allocation would hand out an id something
    // already has.
    if (!m_id.valid()) {
        return;
    }
    if (Insert* insert = project.mixer.find(m_insert)) {
        std::erase_if(insert->slots, [this](const Slot& slot) { return slot.id == m_id; });
    }
    project.restoreIdMarks(m_marks);
    m_id = core::SlotId{};
}

std::string_view AddSlot::name() const noexcept {
    return "Add effect";
}

CommandId AddSlot::kind() const noexcept {
    return CommandId::kAddSlot;
}

DirtyMask AddSlot::dirty() const noexcept {
    return dirty::kMixer;
}

// --- RemoveSlot --------------------------------------------------------------

RemoveSlot::RemoveSlot(core::SlotId id) : m_id(id) {}

void RemoveSlot::apply(Project& project) {
    m_insert = project.mixer.ownerOfSlot(m_id);
    Insert* insert = project.mixer.find(m_insert);
    if (insert == nullptr) {
        m_removed = false;
        return;
    }
    const auto match = std::ranges::find(insert->slots, m_id, &Slot::id);
    m_removed = match != insert->slots.end();
    if (!m_removed) {
        return;
    }
    m_index = static_cast<std::size_t>(std::distance(insert->slots.begin(), match));
    m_previous = *match;
    insert->slots.erase(match);
}

void RemoveSlot::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    Insert* insert = project.mixer.find(m_insert);
    if (insert == nullptr) {
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, insert->slots.size()));
    insert->slots.insert(insert->slots.begin() + at, m_previous);
}

std::string_view RemoveSlot::name() const noexcept {
    return "Remove effect";
}

CommandId RemoveSlot::kind() const noexcept {
    return CommandId::kRemoveSlot;
}

DirtyMask RemoveSlot::dirty() const noexcept {
    return dirty::kMixer;
}

// --- SetSlotValue ------------------------------------------------------------

SetSlotValue::SetSlotValue(core::SlotId id, SlotField field, double value)
    : m_id(id), m_field(field), m_value(value) {}

SetSlotValue::SetSlotValue(core::SlotId id, std::string paramName, double value)
    : m_id(id), m_param(std::move(paramName)), m_value(value) {}

void SetSlotValue::apply(Project& project) {
    Slot* slot = project.mixer.findSlot(m_id);
    if (slot == nullptr) {
        return;
    }
    if (m_param.empty()) {
        if (m_field == SlotField::Mix) {
            m_previous = slot->mix;
        } else {
            m_previous = slot->bypass ? 1.0 : 0.0;
        }
        if (m_field == SlotField::Mix) {
            slot->mix = static_cast<float>(m_value);
        } else {
            slot->bypass = m_value != 0.0;
        }
        return;
    }
    SlotParam* existing = slot->find(m_param);
    m_existed = existing != nullptr;
    if (m_existed) {
        m_previous = existing->value;
        existing->value = m_value;
    } else {
        slot->params.push_back(SlotParam{.name = m_param, .value = m_value});
    }
}

void SetSlotValue::revert(Project& project) {
    Slot* slot = project.mixer.findSlot(m_id);
    if (slot == nullptr) {
        return;
    }
    if (m_param.empty()) {
        if (m_field == SlotField::Mix) {
            slot->mix = static_cast<float>(m_previous);
        } else {
            slot->bypass = m_previous != 0.0;
        }
        return;
    }
    if (m_existed) {
        if (SlotParam* existing = slot->find(m_param)) {
            existing->value = m_previous;
        }
        return;
    }
    std::erase_if(slot->params, [this](const SlotParam& param) { return param.name == m_param; });
}

std::string_view SetSlotValue::name() const noexcept {
    return "Set effect value";
}

CommandId SetSlotValue::kind() const noexcept {
    return CommandId::kSetSlotValue;
}

DirtyMask SetSlotValue::dirty() const noexcept {
    return dirty::kMixer;
}

std::uint64_t SetSlotValue::targetKey() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : m_param) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 1099511628211ULL;
    }
    return hash ^ (static_cast<std::uint64_t>(m_field) << 40U) ^ m_id.value;
}

bool SetSlotValue::coalesceWith(const Command& next) {
    const auto& other = static_cast<const SetSlotValue&>(next);
    if (other.m_param != m_param || other.m_field != m_field) {
        return false;
    }
    m_value = other.m_value;
    return true;
}

// --- AddSend -----------------------------------------------------------------

AddSend::AddSend(core::InsertId insert, core::InsertId target, float level, bool preFader,
                 core::SendId wanted)
    : m_insert(insert), m_target(target), m_level(level), m_preFader(preFader), m_wanted(wanted) {}

void AddSend::apply(Project& project) {
    Insert* insert = project.mixer.find(m_insert);
    if (insert == nullptr) {
        return;
    }
    m_marks = project.idMarks();
    m_id = project.newSendId(m_wanted);
    insert->sends.push_back(
        Send{.id = m_id, .target = m_target, .level = m_level, .preFader = m_preFader});
}

void AddSend::revert(Project& project) {
    if (!m_id.valid()) {
        return;
    }
    if (Insert* insert = project.mixer.find(m_insert)) {
        std::erase_if(insert->sends, [this](const Send& send) { return send.id == m_id; });
    }
    project.restoreIdMarks(m_marks);
    m_id = core::SendId{};
}

std::string_view AddSend::name() const noexcept {
    return "Add send";
}

CommandId AddSend::kind() const noexcept {
    return CommandId::kAddSend;
}

DirtyMask AddSend::dirty() const noexcept {
    return dirty::kMixer | dirty::kRouting;
}

// --- RemoveSend --------------------------------------------------------------

RemoveSend::RemoveSend(core::SendId id) : m_id(id) {}

void RemoveSend::apply(Project& project) {
    m_insert = project.mixer.ownerOfSend(m_id);
    Insert* insert = project.mixer.find(m_insert);
    if (insert == nullptr) {
        m_removed = false;
        return;
    }
    const auto match = std::ranges::find(insert->sends, m_id, &Send::id);
    m_removed = match != insert->sends.end();
    if (!m_removed) {
        return;
    }
    m_index = static_cast<std::size_t>(std::distance(insert->sends.begin(), match));
    m_previous = *match;
    insert->sends.erase(match);
}

void RemoveSend::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    Insert* insert = project.mixer.find(m_insert);
    if (insert == nullptr) {
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, insert->sends.size()));
    insert->sends.insert(insert->sends.begin() + at, m_previous);
}

std::string_view RemoveSend::name() const noexcept {
    return "Remove send";
}

CommandId RemoveSend::kind() const noexcept {
    return CommandId::kRemoveSend;
}

DirtyMask RemoveSend::dirty() const noexcept {
    return dirty::kMixer | dirty::kRouting;
}

// --- SetSendLevel ------------------------------------------------------------

SetSendLevel::SetSendLevel(core::SendId id, float level) : m_id(id), m_level(level) {}

void SetSendLevel::apply(Project& project) {
    Send* send = project.mixer.findSend(m_id);
    if (send == nullptr) {
        return;
    }
    m_previous = send->level;
    send->level = m_level;
}

void SetSendLevel::revert(Project& project) {
    if (Send* send = project.mixer.findSend(m_id)) {
        send->level = m_previous;
    }
}

std::string_view SetSendLevel::name() const noexcept {
    return "Set send level";
}

CommandId SetSendLevel::kind() const noexcept {
    return CommandId::kSetSendLevel;
}

DirtyMask SetSendLevel::dirty() const noexcept {
    return dirty::kMixer;
}

std::uint64_t SetSendLevel::targetKey() const noexcept {
    return m_id.value;
}

bool SetSendLevel::coalesceWith(const Command& next) {
    m_level = static_cast<const SetSendLevel&>(next).m_level;
    return true;
}

// --- AddRoute ----------------------------------------------------------------

AddRoute::AddRoute(core::InsertId from, core::InsertId to, core::RouteId wanted)
    : m_from(from), m_to(to), m_wanted(wanted) {}

void AddRoute::apply(Project& project) {
    m_marks = project.idMarks();
    m_id = project.newRouteId(m_wanted);
    project.mixer.routes.push_back(Route{.id = m_id, .from = m_from, .to = m_to});
}

void AddRoute::revert(Project& project) {
    std::erase_if(project.mixer.routes, [this](const Route& route) { return route.id == m_id; });
    project.restoreIdMarks(m_marks);
    m_id = core::RouteId{};
}

std::string_view AddRoute::name() const noexcept {
    return "Add route";
}

CommandId AddRoute::kind() const noexcept {
    return CommandId::kAddRoute;
}

DirtyMask AddRoute::dirty() const noexcept {
    return dirty::kRouting;
}

// --- RemoveRoute -------------------------------------------------------------

RemoveRoute::RemoveRoute(core::RouteId id) : m_id(id) {}

void RemoveRoute::apply(Project& project) {
    auto& routes = project.mixer.routes;
    const auto match = std::ranges::find(routes, m_id, &Route::id);
    m_removed = match != routes.end();
    if (!m_removed) {
        return;
    }
    m_index = static_cast<std::size_t>(std::distance(routes.begin(), match));
    m_previous = *match;
    routes.erase(match);
}

void RemoveRoute::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    auto& routes = project.mixer.routes;
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, routes.size()));
    routes.insert(routes.begin() + at, m_previous);
}

std::string_view RemoveRoute::name() const noexcept {
    return "Remove route";
}

CommandId RemoveRoute::kind() const noexcept {
    return CommandId::kRemoveRoute;
}

DirtyMask RemoveRoute::dirty() const noexcept {
    return dirty::kRouting;
}

// --- SetMaster ---------------------------------------------------------------

SetMaster::SetMaster(core::InsertId id) : m_id(id) {}

void SetMaster::apply(Project& project) {
    m_previous = project.mixer.master;
    project.mixer.master = m_id;
}

void SetMaster::revert(Project& project) {
    project.mixer.master = m_previous;
}

std::string_view SetMaster::name() const noexcept {
    return "Set master insert";
}

CommandId SetMaster::kind() const noexcept {
    return CommandId::kSetMaster;
}

DirtyMask SetMaster::dirty() const noexcept {
    return dirty::kMixer | dirty::kRouting;
}

} // namespace adx::project
