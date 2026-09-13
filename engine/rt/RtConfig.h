// Constants and markers shared by everything on the audio path.
//
// Includes nothing but <cstddef>/<cstdint> on purpose: the realtime ban list
// (.clang-tidy-rt) bans a type by banning its header, transitively, so anything a
// realtime header pulls in becomes unbannable for every file that includes it.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/Config.h"

namespace adx::rt {

/// The largest block a backend may hand the engine in one callback.
///
/// Every fixed-capacity buffer on the audio path is sized from this, so it is the
/// number that decides how much memory the realtime side reserves up front. 2048
/// frames is ~43 ms at 48 kHz - far beyond any sane device setting, which is the
/// point: exceeding it is a bug, not a configuration.
inline constexpr std::uint32_t kMaxBlockFrames = 2048;

/// Assumed cache-line size, used to keep a producer's and a consumer's counters
/// off the same line. False sharing between the UI thread and the audio thread on
/// a 50 us budget is not theoretical.
///
/// std::hardware_destructive_interference_size would be the standard spelling, but
/// MSVC reports 64 while warning that using it in a header is an ABI hazard, and
/// every x86-64 part this targets has a 64-byte line.
inline constexpr std::size_t kCacheLine = 64;

} // namespace adx::rt

/// Marks a function that runs inside the audio callback.
///
/// It forces inlining, but the reason it exists is documentary: a function
/// carrying this marker is bound by FINAL_PLAN Â§2.2 Rule 1 - no allocation, no
/// locks, no exceptions, no unbounded work - and is expected to be noexcept.
#define ADX_RT_HOT ADX_FORCE_INLINE
