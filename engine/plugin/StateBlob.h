// adx-thread: main
//
// A plugin's own opaque state, and its text form in the .adx file (phase_9.md §4.7):
// `base64:` and the standard alphabet with padding. The blob is the irreducible
// remainder after the named parameters, so it stays on a line of its own.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adx::plugin {

using StateBlob = std::vector<std::uint8_t>;

inline constexpr std::string_view kStatePrefix = "base64:";

[[nodiscard]] std::string encodeBase64(std::span<const std::uint8_t> bytes);
/// False for anything that is not canonical base64 (wrong length, a stray character).
[[nodiscard]] bool decodeBase64(std::string_view text, StateBlob& out);

} // namespace adx::plugin
