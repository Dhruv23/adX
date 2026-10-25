// adx-thread: main
//
// What a reload may and may not disturb (phase_7.md §4.4). The unifying rule: **a
// reload never cuts a sounding voice.** It releases it.
//
// | Change in the file                       | Effect on playback                          |
// |------------------------------------------|---------------------------------------------|
// | note added/removed/changed, future tick  | none; the scheduler's cursor picks it up    |
// | note changed at the current tick         | takes effect next block                     |
// | parameter value                          | the node ramps to it (Phase 4's smoothing)  |
// | effect parameter, same effect            | same; the instance and its state persist    |
// | effect replaced by another type          | new instance; the old one's tail crossfades |
// |                                          | out over kEffectSwapFadeMs                  |
// | channel instrument changed               | the old voices are released and ring out    |
// |                                          | (at most kInstrumentReleaseCapMs); new      |
// |                                          | notes play on the new instrument            |
// | channel removed                          | its node leaves the graph: voices are cut   |
// |                                          | (P7-3 - the one row not yet met)            |
// | tempo changed                            | next block; positions follow the new map    |
// | anything structural (routing, inserts)   | one snapshot rebuild, swapped between       |
// |                                          | blocks; every surviving node is reused      |
//
// How each row is met:
//
//   - Patch.h remaps the new text into the open project's ids, so an entity that
//     survives an edit keeps its id - and NodeStore keys nodes by id, so it keeps its
//     node, its voices and its tail. That is most of the table.
//   - The two rows a new node cannot avoid - a new effect type, a new instrument -
//     are graph::Handover.h: the new node is handed the old one.
//   - A file that does not parse cleanly is not applied at all (reloadMayApply): a
//     half-typed line must not delete the channel it is in.
#pragma once

#include "engine/format/adx/Diagnostics.h"
#include "engine/graph/Handover.h"

namespace adx::sync {

using graph::kEffectSwapFadeMs;
using graph::kInstrumentReleaseCapMs;

/// How long the file watcher waits for quiet before reloading (phase_7.md §4.4).
inline constexpr int kWatcherDebounceMs = 150;
/// How long Live mode waits after the last edit before writing the file.
inline constexpr int kLiveWriteDebounceMs = 300;

/// Text with errors is shown, never applied: the model keeps what it had until the
/// text says something whole again. Warnings do not block.
[[nodiscard]] inline bool reloadMayApply(const format::DiagnosticList& diagnostics) noexcept {
    return !diagnostics.hasErrors();
}

} // namespace adx::sync
