// adx-thread: main
#include "engine/midi/dx7/Dx7ParamChange.h"

#include "engine/instruments/dx7/Dx7Params.h"

namespace adx::dx7 {

namespace {

constexpr std::uint8_t kGroupVoice = 0;
constexpr std::uint8_t kGroupFunction = 2;
constexpr std::uint32_t kFunctionFirst = 64;
constexpr std::uint32_t kFunctionCount =
    paramIndex(Dx7Param::AftertouchAssign) - paramIndex(Dx7Param::Mono) + 1; // 14

} // namespace

std::optional<std::array<std::uint8_t, 7>> encodeParamChange(const ParamChange& change) noexcept {
    std::uint8_t group = 0;
    std::uint32_t number = 0;
    if (change.param < kVoiceBytes) {
        group = kGroupVoice;
        number = change.param;
    } else if (change.param >= paramIndex(Dx7Param::Mono) &&
               change.param - paramIndex(Dx7Param::Mono) < kFunctionCount) {
        group = kGroupFunction;
        number = kFunctionFirst + (change.param - paramIndex(Dx7Param::Mono));
    } else {
        return std::nullopt;
    }
    return std::array<std::uint8_t, 7>{
        0xF0,
        0x43,
        static_cast<std::uint8_t>(0x10 | (change.channel & 0x0F)),
        static_cast<std::uint8_t>((group << 2) | ((number >> 7) & 3)),
        static_cast<std::uint8_t>(number & 0x7F),
        static_cast<std::uint8_t>(change.value & 0x7F),
        0xF7};
}

std::optional<ParamChange> decodeParamChange(std::span<const std::uint8_t> m) noexcept {
    if (m.size() != 7 || m[0] != 0xF0 || m[1] != 0x43 || (m[2] & 0xF0) != 0x10 || m[6] != 0xF7) {
        return std::nullopt;
    }
    const auto group = static_cast<std::uint8_t>((m[3] >> 2) & 0x1F);
    const std::uint32_t number = (static_cast<std::uint32_t>(m[3] & 3) << 7) | (m[4] & 0x7FU);
    ParamChange change{.param = 0,
                       .value = static_cast<std::uint8_t>(m[5] & 0x7F),
                       .channel = static_cast<std::uint8_t>(m[2] & 0x0F)};
    if (group == kGroupVoice && number < kVoiceBytes) {
        change.param = number;
        return change;
    }
    if (group == kGroupFunction && number >= kFunctionFirst &&
        number < kFunctionFirst + kFunctionCount) {
        change.param = paramIndex(Dx7Param::Mono) + (number - kFunctionFirst);
        return change;
    }
    return std::nullopt;
}

} // namespace adx::dx7
