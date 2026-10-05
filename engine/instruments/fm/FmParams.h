// The FM instrument's parameters, in the order its node reads them (phase_4.md §4.8).
//
// Four globals, then six operators of ten parameters each: parameter
// kFmGlobalParams + op * kFmOpParams + field, with op 0 = operator 1. The table itself
// is in FmParams.cpp; it is 64 rows, written out so each name is a literal.
#pragma once

#include <array>
#include <cstdint>

#include "engine/project/ParamDescriptor.h"

namespace adx::instruments {

inline constexpr std::uint32_t kFmOperators = 6;
inline constexpr std::uint32_t kFmAlgorithms = 32;

enum class FmParam : std::uint32_t {
    /// 1..32, the DX7's numbering.
    Algorithm,
    /// The algorithm's feedback loop, 0..1.
    Feedback,
    Level,
    Glide,
    Count,
};
inline constexpr std::uint32_t kFmGlobalParams = static_cast<std::uint32_t>(FmParam::Count);

enum class FmOpParam : std::uint32_t {
    /// Frequency as a multiple of the note's.
    Ratio,
    /// 1: the operator ignores the note and runs at `freq`.
    Fixed,
    Freq,
    Detune,
    /// Output level: for a carrier, loudness; for a modulator, modulation depth.
    Level,
    Attack,
    Decay,
    Sustain,
    Release,
    /// How much velocity scales the level: 0 none, 1 linear.
    Velocity,
    Count,
};
inline constexpr std::uint32_t kFmOpParams = static_cast<std::uint32_t>(FmOpParam::Count);
inline constexpr std::uint32_t kFmParamCount = kFmGlobalParams + (kFmOperators * kFmOpParams);

[[nodiscard]] constexpr std::uint32_t fmOpParam(std::uint32_t op, FmOpParam field) noexcept {
    return kFmGlobalParams + (op * kFmOpParams) + static_cast<std::uint32_t>(field);
}

/// One descriptor per parameter, in index order (FmParams.cpp).
extern const std::array<project::ParamDescriptor, kFmParamCount> kFmParams;

} // namespace adx::instruments
