#include "engine/project/commands/ChannelCommands.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace adx::project {
namespace {

[[nodiscard]] double readField(const Channel& channel, ChannelField field) noexcept {
    switch (field) {
    case ChannelField::Volume:
        return channel.volume;
    case ChannelField::Pan:
        return channel.pan;
    case ChannelField::PitchCents:
        return channel.pitchOffsetCents;
    case ChannelField::Muted:
        return channel.muted ? 1.0 : 0.0;
    case ChannelField::Soloed:
        return channel.soloed ? 1.0 : 0.0;
    case ChannelField::MaxPolyphony:
        return channel.maxPolyphony;
    case ChannelField::StealMode:
        return static_cast<double>(channel.stealMode);
    }
    return 0.0;
}

void writeField(Channel& channel, ChannelField field, double value) noexcept {
    switch (field) {
    case ChannelField::Volume:
        channel.volume = static_cast<float>(value);
        break;
    case ChannelField::Pan:
        channel.pan = static_cast<float>(value);
        break;
    case ChannelField::PitchCents:
        channel.pitchOffsetCents = static_cast<float>(value);
        break;
    case ChannelField::Muted:
        channel.muted = value != 0.0;
        break;
    case ChannelField::Soloed:
        channel.soloed = value != 0.0;
        break;
    case ChannelField::MaxPolyphony:
        channel.maxPolyphony = static_cast<std::uint16_t>(std::clamp(value, 1.0, 65535.0));
        break;
    case ChannelField::StealMode:
        channel.stealMode = static_cast<VoiceStealMode>(
            std::clamp(value, 0.0, static_cast<double>(kVoiceStealModeCount - 1)));
        break;
    }
}

} // namespace

// --- AddChannel --------------------------------------------------------------

AddChannel::AddChannel(Channel prototype) : m_prototype(std::move(prototype)) {}

void AddChannel::apply(Project& project) {
    m_marks = project.idMarks();
    m_id = project.newChannelId(m_prototype.id);
    Channel channel = m_prototype;
    channel.id = m_id;
    project.channels.push_back(std::move(channel));
}

void AddChannel::revert(Project& project) {
    std::erase_if(project.channels, [this](const Channel& c) { return c.id == m_id; });
    project.restoreIdMarks(m_marks);
    m_id = core::ChannelId{};
}

std::string_view AddChannel::name() const noexcept {
    return "Add channel";
}

CommandId AddChannel::kind() const noexcept {
    return CommandId::kAddChannel;
}

DirtyMask AddChannel::dirty() const noexcept {
    return dirty::kChannels;
}

// --- RemoveChannel -----------------------------------------------------------

RemoveChannel::RemoveChannel(core::ChannelId id) : m_id(id) {}

void RemoveChannel::apply(Project& project) {
    const auto match = std::ranges::find(project.channels, m_id, &Channel::id);
    m_removed = match != project.channels.end();
    if (!m_removed) {
        return;
    }
    m_index = static_cast<std::size_t>(std::distance(project.channels.begin(), match));
    m_previous = *match;
    project.channels.erase(match);

    m_clips.clear();
    m_minis.clear();
    for (Pattern& pattern : project.patterns) {
        for (std::size_t i = pattern.noteClips.size(); i-- > 0;) {
            if (pattern.noteClips[i].channel != m_id) {
                continue;
            }
            m_clips.push_back(
                DetachedClip{.pattern = pattern.id, .index = i, .clip = pattern.noteClips[i]});
            pattern.noteClips.erase(pattern.noteClips.begin() + static_cast<std::ptrdiff_t>(i));
        }
        for (std::size_t i = pattern.mini.size(); i-- > 0;) {
            if (pattern.mini[i].channel != m_id) {
                continue;
            }
            m_minis.push_back(
                DetachedMini{.pattern = pattern.id, .index = i, .source = pattern.mini[i]});
            pattern.mini.erase(pattern.mini.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

void RemoveChannel::revert(Project& project) {
    if (!m_removed) {
        return;
    }
    const auto at = static_cast<std::ptrdiff_t>(std::min(m_index, project.channels.size()));
    project.channels.insert(project.channels.begin() + at, m_previous);

    // Detached in descending index order and restored in ascending order - the
    // reverse of collection. Restoring in collection order looks right and is not:
    // removing indices 1 and 2 of four and putting them back highest-first swaps
    // them, which undo_to_empty_random caught.
    for (const DetachedClip& detached : std::views::reverse(m_clips)) {
        Pattern* pattern = project.find(detached.pattern);
        if (pattern == nullptr) {
            continue;
        }
        const auto index =
            static_cast<std::ptrdiff_t>(std::min(detached.index, pattern->noteClips.size()));
        pattern->noteClips.insert(pattern->noteClips.begin() + index, detached.clip);
    }
    for (const DetachedMini& detached : std::views::reverse(m_minis)) {
        Pattern* pattern = project.find(detached.pattern);
        if (pattern == nullptr) {
            continue;
        }
        const auto index =
            static_cast<std::ptrdiff_t>(std::min(detached.index, pattern->mini.size()));
        pattern->mini.insert(pattern->mini.begin() + index, detached.source);
    }
}

std::string_view RemoveChannel::name() const noexcept {
    return "Remove channel";
}

CommandId RemoveChannel::kind() const noexcept {
    return CommandId::kRemoveChannel;
}

DirtyMask RemoveChannel::dirty() const noexcept {
    return dirty::kChannels | dirty::kPatterns;
}

// --- SetChannelValue ---------------------------------------------------------

SetChannelValue::SetChannelValue(core::ChannelId id, ChannelField field, double value)
    : m_id(id), m_field(field), m_value(value) {}

void SetChannelValue::apply(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel == nullptr) {
        return;
    }
    m_previous = readField(*channel, m_field);
    writeField(*channel, m_field, m_value);
}

void SetChannelValue::revert(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel != nullptr) {
        writeField(*channel, m_field, m_previous);
    }
}

std::string_view SetChannelValue::name() const noexcept {
    return "Set channel value";
}

CommandId SetChannelValue::kind() const noexcept {
    return CommandId::kSetChannelValue;
}

DirtyMask SetChannelValue::dirty() const noexcept {
    return dirty::kChannels;
}

std::uint64_t SetChannelValue::targetKey() const noexcept {
    // Field in the high bits: dragging the volume fader and then the pan knob within
    // the window are two edits, not one.
    return (static_cast<std::uint64_t>(m_field) << 32U) | m_id.value;
}

bool SetChannelValue::coalesceWith(const Command& next) {
    m_value = static_cast<const SetChannelValue&>(next).m_value;
    return true;
}

// --- RenameChannel -----------------------------------------------------------

RenameChannel::RenameChannel(core::ChannelId id, std::string channelName)
    : m_id(id), m_name(std::move(channelName)) {}

void RenameChannel::apply(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel == nullptr) {
        return;
    }
    m_previous = channel->name;
    channel->name = m_name;
}

void RenameChannel::revert(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel != nullptr) {
        channel->name = m_previous;
    }
}

std::string_view RenameChannel::name() const noexcept {
    return "Rename channel";
}

CommandId RenameChannel::kind() const noexcept {
    return CommandId::kRenameChannel;
}

DirtyMask RenameChannel::dirty() const noexcept {
    return dirty::kChannels;
}

std::uint64_t RenameChannel::targetKey() const noexcept {
    return m_id.value;
}

bool RenameChannel::coalesceWith(const Command& next) {
    m_name = static_cast<const RenameChannel&>(next).m_name;
    return true;
}

// --- SetChannelColor ---------------------------------------------------------

SetChannelColor::SetChannelColor(core::ChannelId id, Color color) : m_id(id), m_color(color) {}

void SetChannelColor::apply(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel == nullptr) {
        return;
    }
    m_previous = channel->color;
    channel->color = m_color;
}

void SetChannelColor::revert(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel != nullptr) {
        channel->color = m_previous;
    }
}

std::string_view SetChannelColor::name() const noexcept {
    return "Set channel colour";
}

CommandId SetChannelColor::kind() const noexcept {
    return CommandId::kSetChannelValue;
}

DirtyMask SetChannelColor::dirty() const noexcept {
    return dirty::kChannels;
}

std::uint64_t SetChannelColor::targetKey() const noexcept {
    // Deliberately distinct from every ChannelField key so a colour picker drag does
    // not merge into a fader drag on the same channel.
    return (static_cast<std::uint64_t>(0xC0) << 32U) | m_id.value;
}

bool SetChannelColor::coalesceWith(const Command& next) {
    m_color = static_cast<const SetChannelColor&>(next).m_color;
    return true;
}

// --- SetChannelOutput --------------------------------------------------------

SetChannelOutput::SetChannelOutput(core::ChannelId id, core::InsertId output)
    : m_id(id), m_output(output) {}

void SetChannelOutput::apply(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel == nullptr) {
        return;
    }
    m_previous = channel->output;
    channel->output = m_output;
}

void SetChannelOutput::revert(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel != nullptr) {
        channel->output = m_previous;
    }
}

std::string_view SetChannelOutput::name() const noexcept {
    return "Route channel";
}

CommandId SetChannelOutput::kind() const noexcept {
    return CommandId::kSetChannelOutput;
}

DirtyMask SetChannelOutput::dirty() const noexcept {
    return dirty::kChannels | dirty::kRouting;
}

// --- SetInstrumentType -------------------------------------------------------

SetInstrumentType::SetInstrumentType(core::ChannelId id, std::string type)
    : m_id(id), m_type(std::move(type)) {}

void SetInstrumentType::apply(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel == nullptr) {
        return;
    }
    m_previous = channel->instrument.type;
    channel->instrument.type = m_type;
}

void SetInstrumentType::revert(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel != nullptr) {
        channel->instrument.type = m_previous;
    }
}

std::string_view SetInstrumentType::name() const noexcept {
    return "Set instrument";
}

CommandId SetInstrumentType::kind() const noexcept {
    return CommandId::kSetChannelValue;
}

DirtyMask SetInstrumentType::dirty() const noexcept {
    return dirty::kChannels;
}

// --- SetChannelParam ---------------------------------------------------------

SetChannelParam::SetChannelParam(core::ChannelId id, std::string paramName, double value)
    : m_id(id), m_value(ParamValue{
                    .name = std::move(paramName), .value = value, .hasCurve = false, .curve = {}}) {
}

SetChannelParam::SetChannelParam(core::ChannelId id, std::string paramName, double value,
                                 core::Curve curve)
    : m_id(id),
      m_value(ParamValue{
          .name = std::move(paramName), .value = value, .hasCurve = true, .curve = curve}) {}

void SetChannelParam::apply(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel == nullptr) {
        return;
    }
    ParamValue* existing = channel->instrument.find(m_value.name);
    m_existed = existing != nullptr;
    if (m_existed) {
        m_previous = *existing;
        *existing = m_value;
    } else {
        channel->instrument.params.push_back(m_value);
    }
}

void SetChannelParam::revert(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel == nullptr) {
        return;
    }
    if (m_existed) {
        if (ParamValue* existing = channel->instrument.find(m_value.name)) {
            *existing = m_previous;
        }
        return;
    }
    // Appended, so it is the last one. Erasing by name rather than by position keeps
    // this correct if something else appended after it - which a group can do.
    std::erase_if(channel->instrument.params,
                  [this](const ParamValue& param) { return param.name == m_value.name; });
}

std::string_view SetChannelParam::name() const noexcept {
    return "Set parameter";
}

CommandId SetChannelParam::kind() const noexcept {
    return CommandId::kSetChannelParam;
}

DirtyMask SetChannelParam::dirty() const noexcept {
    return dirty::kChannels;
}

std::uint64_t SetChannelParam::targetKey() const noexcept {
    // Hashing the name into the key: two different parameters on one channel are two
    // history entries, and a collision would only ever merge two edits that happened
    // within half a second of each other.
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : m_value.name) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 1099511628211ULL;
    }
    return hash ^ m_id.value;
}

bool SetChannelParam::coalesceWith(const Command& next) {
    const auto& other = static_cast<const SetChannelParam&>(next);
    if (other.m_value.name != m_value.name) {
        return false;
    }
    m_value.value = other.m_value.value;
    m_value.hasCurve = other.m_value.hasCurve;
    m_value.curve = other.m_value.curve;
    return true;
}

// --- SetChannelArp -----------------------------------------------------------

SetChannelArp::SetChannelArp(core::ChannelId id, ArpSettings arp) : m_id(id), m_arp(arp) {}

void SetChannelArp::apply(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel == nullptr) {
        return;
    }
    m_previous = channel->arp;
    channel->arp = m_arp;
}

void SetChannelArp::revert(Project& project) {
    Channel* channel = project.find(m_id);
    if (channel != nullptr) {
        channel->arp = m_previous;
    }
}

std::string_view SetChannelArp::name() const noexcept {
    return "Set arpeggiator";
}

CommandId SetChannelArp::kind() const noexcept {
    return CommandId::kSetChannelArp;
}

DirtyMask SetChannelArp::dirty() const noexcept {
    return dirty::kChannels;
}

std::uint64_t SetChannelArp::targetKey() const noexcept {
    return m_id.value;
}

bool SetChannelArp::coalesceWith(const Command& next) {
    m_arp = static_cast<const SetChannelArp&>(next).m_arp;
    return true;
}

} // namespace adx::project
