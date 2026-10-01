// A loop region, in ticks.
//
// Ticks, not samples, because a loop is a musical span: change the tempo and the
// loop should still be "bars 5 to 9". The time source converts it to samples through
// its own tempo map, which is also why a Phase 11 clip can loop four bars at its own
// tempo while the arrangement loops eight at another.
#pragma once

#include "engine/core/Time.h"

namespace adx::transport {

struct LoopRegion {
    core::Ticks start;
    core::Ticks end;
    bool enabled{false};

    /// Enabled and non-empty. A region whose end is not after its start cannot be
    /// looped - there is nothing to play - so it behaves as if disabled rather than
    /// spinning the scheduler on a zero-length block.
    [[nodiscard]] constexpr bool active() const noexcept {
        return enabled && end > start;
    }

    [[nodiscard]] friend constexpr bool operator==(const LoopRegion&,
                                                   const LoopRegion&) noexcept = default;
};

} // namespace adx::transport
