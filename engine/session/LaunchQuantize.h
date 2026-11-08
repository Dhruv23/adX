// adx-thread: realtime
//
// Launch quantize (phase_11.md §4.3): where on the musical grid a launch or a stop lands.
// Integer tick arithmetic, so a boundary is exact and stays exact across tempo changes;
// the sample it falls on is the tempo map's business, not this file's.
#pragma once

#include <cstdint>

namespace adx::session {

enum class LaunchQuantize : std::uint8_t {
    /// The session's global setting (a slot's default).
    Global,
    /// The next sample: one-shots.
    Instant,
    Quarter,
    Half,
    Bar,
    TwoBars,
    FourBars,
    EightBars,
};

/// The grid step `quantize` names, in ticks, with `barTicks` a bar; 0 for Instant (and
/// for Global, which a caller resolves first).
[[nodiscard]] std::int64_t quantizeTicks(LaunchQuantize quantize, std::int64_t barTicks) noexcept;

/// The first grid boundary at or after `tick`, the grid running from `origin` in steps of
/// quantizeTicks(). Instant (a step of 0) is `tick` itself.
[[nodiscard]] std::int64_t nextBoundary(std::int64_t tick, LaunchQuantize quantize,
                                        std::int64_t barTicks, std::int64_t origin = 0) noexcept;

/// Names as the [SESSION] section spells them: global, instant, 1/4, 1/2, bar, 2bar,
/// 4bar, 8bar. parseQuantize returns false for anything else.
[[nodiscard]] const char* toString(LaunchQuantize quantize) noexcept;
bool parseQuantize(const char* text, LaunchQuantize& out) noexcept;

} // namespace adx::session
