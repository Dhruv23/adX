// adx-thread: main
//
// Count-in and pre-roll (phase_8.md §2): where the transport starts before a
// recording, and whether the click plays while it does. Arithmetic on the tempo map,
// nothing more - the click is the engine's metronome (RenderEngine::setMetronome).
#pragma once

#include <algorithm>
#include <cstdint>

#include "engine/core/TempoMap.h"
#include "engine/core/Time.h"

namespace adx::record {

struct CountIn {
    /// Bars of pre-roll before the record point; 0 starts on it.
    std::uint32_t bars{1};
    /// Play the click during the pre-roll and the take.
    bool click{true};
};

/// Where the transport starts so that `bars` bars - in the meter at `recordAt` - pass
/// before it. Never before the start of the song.
[[nodiscard]] inline core::Ticks preRollStart(const core::TempoMap& tempo, core::Ticks recordAt,
                                              const CountIn& countIn) {
    const core::MeterEvent meter = tempo.meterAt(recordAt);
    const std::int64_t bar = core::ticksPerBeat(meter.denominator) * meter.numerator;
    return core::Ticks{std::max<std::int64_t>(0, recordAt.value - (bar * countIn.bars))};
}

} // namespace adx::record
