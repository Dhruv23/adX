#include "engine/project/Channel.h"

#include <algorithm>
#include <array>

namespace adx::project {
namespace {

/// Indexed by ArpMode. Order must match the enum.
constexpr std::array<std::string_view, kArpModeCount> kArpModeNames{
    "off", "up", "down", "updown", "downup", "random", "order",
};
static_assert(kArpModeNames.size() == kArpModeCount);

constexpr std::array<std::string_view, kVoiceStealModeCount> kStealNames{
    "oldest-released",
    "oldest",
    "quietest",
    "none",
};
static_assert(kStealNames.size() == kVoiceStealModeCount);

template<std::size_t N>
[[nodiscard]] bool lookupName(const std::array<std::string_view, N>& table, std::string_view name,
                              std::size_t& out) noexcept {
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table.at(i) == name) {
            out = i;
            return true;
        }
    }
    return false;
}

} // namespace

const ParamValue* InstrumentSpec::find(std::string_view name) const noexcept {
    const auto match = std::ranges::find(params, name, &ParamValue::name);
    return match == params.end() ? nullptr : &*match;
}

ParamValue* InstrumentSpec::find(std::string_view name) noexcept {
    const auto match = std::ranges::find(params, name, &ParamValue::name);
    return match == params.end() ? nullptr : &*match;
}

const char* toString(ArpMode mode) noexcept {
    const auto index = static_cast<std::size_t>(mode);
    return index < kArpModeNames.size() ? kArpModeNames.at(index).data() : "off";
}

bool arpModeFromString(std::string_view name, ArpMode& out) noexcept {
    std::size_t index = 0;
    if (!lookupName(kArpModeNames, name, index)) {
        return false;
    }
    out = static_cast<ArpMode>(index);
    return true;
}

const char* toString(VoiceStealMode mode) noexcept {
    const auto index = static_cast<std::size_t>(mode);
    return index < kStealNames.size() ? kStealNames.at(index).data() : "oldest-released";
}

bool voiceStealModeFromString(std::string_view name, VoiceStealMode& out) noexcept {
    std::size_t index = 0;
    if (!lookupName(kStealNames, name, index)) {
        return false;
    }
    out = static_cast<VoiceStealMode>(index);
    return true;
}

} // namespace adx::project
