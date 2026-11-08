// adx-thread: realtime
//
// What the session did, as the performance recorder hears it (phase_11.md §4.6): the
// audio thread logs each pass of each clip, each stop and each rate change at the grid
// tick it happened on, and the main thread turns the log into an ordinary arrangement
// (record/PerformanceRecorder.h). Ticks, not samples: a launch lands on a grid tick by
// construction, so the arrangement can put an item exactly there.
#pragma once

#include <cstdint>
#include <type_traits>

namespace adx::session {

struct PerformanceEvent {
    enum class Kind : std::uint8_t {
        /// A clip's pass began at `tick`: the cell (track, row) from its own tick
        /// `phase` (non-zero only for a legato launch), at most `length - phase` long.
        Pass,
        /// The track's clip stopped at `tick`.
        Stop,
        /// The master rate became `rate` at `tick` (a nudge, a tap, a scene's tempo).
        Rate,
    };
    Kind kind{Kind::Pass};
    std::uint32_t track{0};
    std::uint32_t row{0};
    std::int64_t tick{0};
    std::int64_t phase{0};
    std::int64_t length{0};
    float gain{1.0F};
    double rate{1.0};
};

static_assert(std::is_trivially_copyable_v<PerformanceEvent>);

} // namespace adx::session
