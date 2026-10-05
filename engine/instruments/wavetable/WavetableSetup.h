// adx-thread: main
//
// The Wavetable instrument's main-thread half: the built-in tables and importing a
// channel's own `.wav` table.
#pragma once

#include <filesystem>

#include "engine/instruments/wavetable/WavetableInstrument.h"

namespace adx::project {
struct Channel;
struct Resources;
} // namespace adx::project

namespace adx::instruments {

/// Gives `node` the table in the channel's first zone's file, decoded and cut into
/// 2048-sample frames now - a table is small, a few hundred kilobytes at most, and
/// its frames must exist before the audio thread reads them. Leaves the node without
/// one when there is no zone or the file does not decode; the User bank then plays
/// the Classic table.
void configureWavetable(WavetableInstrument& node, const project::Channel& channel,
                        const project::Resources* resources);

/// True when `node` was built from the file `channel` now names.
[[nodiscard]] bool wavetableMatches(const WavetableInstrument& node,
                                    const project::Channel& channel,
                                    const project::Resources* resources);

/// Builds both morph forms of a table from single cycles of `frameLength` samples.
[[nodiscard]] WavetablePair buildWavetablePair(std::span<const float> cycles,
                                               std::size_t frameLength);

} // namespace adx::instruments
