// adx-thread: main
//
// DX7 single-parameter changes, both ways (phase_11.5.md §4.6): F0 43 1n gg pp vv F7.
// The third byte carries the parameter group in bits 2-3 (0 voice, 2 function) and the
// parameter number's top bits in bits 0-1; pp is its low seven bits. Voice parameters
// 0..155 are the voice bytes; function parameters 64..77 are the DX7 function block
// (poly/mono, pitch bend, portamento, the four controllers' ranges and assigns), which
// map to the dx7 instrument's Dx7Param::Mono..AftertouchAssign. The value is the byte.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace adx::dx7 {

struct ParamChange {
    /// The dx7 parameter index (a voice byte, or Dx7Param::Mono and on).
    std::uint32_t param{0};
    /// The value as a byte (a voice byte's raw value; a function parameter's number).
    std::uint8_t value{0};
    std::uint8_t channel{0};
};

/// The message for a change; nullopt for a parameter with no DX7 address.
[[nodiscard]] std::optional<std::array<std::uint8_t, 7>>
encodeParamChange(const ParamChange& change) noexcept;
/// A received message; nullopt when it is not a DX7 parameter change adX knows.
[[nodiscard]] std::optional<ParamChange>
decodeParamChange(std::span<const std::uint8_t> message) noexcept;

} // namespace adx::dx7
