// Pre-sorted per-channel events, and the cursor that walks them.
//
// This is the fix for FINAL_PLAN §3.3 items 2 and 3. Iteration one scanned every note
// in the project on every callback to find the few that started in the block, then
// rescanned the resulting vector once per *frame*. Here the events are flattened and
// sorted once, on the main thread, when the snapshot is built; the audio thread keeps
// a cursor per track and walks forward from it. The amortised cost of a block is the
// number of events that actually occur in it (phase_3.md §4.5).
//
// POD only: this header is read by realtime code and travels inside a render
// snapshot.
#pragma once

#include <cstdint>
#include <span>
#include <type_traits>

namespace adx::project {

/// Ordered so that sorting by (tick, kind) dispatches offs before ons at the same
/// tick: a note that ends exactly where the next begins frees its voice first, so a
/// monophonic line at full polyphony does not steal from itself.
enum class EventKind : std::uint8_t {
    NoteOff,
    Param,
    NoteOn,
};

/// One event on the arrangement timeline, in ticks.
///
/// Ticks, not samples: the same track serves any sample rate and any time source,
/// and the conversion happens per block through the source's own tempo map - which is
/// what lets two sources at two tempos schedule from one scheduler.
struct ScheduledEvent {
    std::int64_t tick{0};
    /// NoteOn only: where the matching NoteOff sits. A voice remembers it, which is
    /// how a loop wrap knows which voices would otherwise never be released.
    std::int64_t endTick{0};
    /// NoteOn/NoteOff: the model's NoteId. Param: the snapshot's parameter index.
    std::uint32_t noteId{0};
    /// The playlist item this event came from. A pattern placed twice plays the same
    /// NoteId twice; without this the two placements' note-offs would release each
    /// other's voices - iteration one's §3.3.4 defect, in a different costume.
    std::uint32_t instance{0};
    float value{0.0F};
    EventKind kind{EventKind::NoteOn};
    std::uint8_t pitch{0};
    std::uint8_t velocity{0};
    std::uint8_t reserved{0};
};

static_assert(std::is_trivially_copyable_v<ScheduledEvent>);

/// One channel's events on the arrangement, sorted by (tick, kind, noteId, instance).
/// Immutable once published.
struct EventTrack {
    std::span<const ScheduledEvent> events;
};

/// Where a track's walk is, for one time source.
///
/// Separate from EventTrack because the track is immutable snapshot data and this is
/// audio-thread state: the snapshot owns one per track, zero-initialised, which is
/// "never positioned" - generations start at 1, so a fresh cursor always searches
/// once before it trusts itself.
struct EventCursor {
    std::uint32_t index{0};
    /// The index of the first event at or after the loop start, cached so a wrap is
    /// an assignment rather than a search.
    std::uint32_t loopIndex{0};
    std::uint64_t seekGeneration{0};
    std::uint64_t loopGeneration{0};
};

static_assert(std::is_trivially_copyable_v<EventCursor>);

} // namespace adx::project
