// adx-thread: main
//
// The model's musical extras, compiled into what the audio thread plays.
//
// Three things the scheduler would otherwise have to interpret per block, done once
// when a channel's events are flattened (phase_4.md §4.0):
//
//   - a note's slide and pitch curve become a chain of PitchGlide events, straight
//     lines only, so every instrument gets slides for free from the voice base;
//   - a note's lyric becomes a Lyric event carrying an index, never a string;
//   - a channel's arpeggiator replaces its note events with the arpeggiated ones.
//
// All of it is a deterministic function of the model, which is what keeps an offline
// render and its realtime capture identical (phase_3.md §4.10).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/project/Channel.h"
#include "engine/project/EventTrack.h"
#include "engine/project/Knot.h"
#include "engine/project/Pattern.h"
#include "engine/project/Snapshot.h"

namespace adx::project {

/// The note-relative pitch offset, in cents, that `extras` describes: the slide and
/// the pitch curve summed, as one straight-line chain. Empty when the note has
/// neither. Exposed for tests.
[[nodiscard]] std::vector<Knot> pitchOffsetChain(const NoteExtras& extras);

/// Appends the PitchGlide and Lyric events for one placed note that sounds from
/// `onTick` to `offTick` (absolute). Glides at or after the note-off are dropped -
/// they could only move a voice in its release, and a trimmed placement cuts them.
/// A lyric is appended to `lyrics` and its event carries the index.
void appendNoteExtras(std::vector<ScheduledEvent>& out, std::vector<std::string>& lyrics,
                      const NoteExtras& extras, std::int64_t onTick, std::int64_t offTick,
                      std::uint32_t noteId, std::uint32_t instance);

/// Voice ids for arpeggiated notes live above this bit, so they can never collide with
/// a model NoteId on the same channel.
inline constexpr std::uint32_t kArpNoteIdBit = 0x80000000U;

/// Replaces the note events in `events` (already sorted) with the arpeggiator's
/// (phase_2.md §4.4's ARP line, which no phase before this one played). Each step on
/// the `rate` grid - measured from tick 0, so a pattern placed anywhere stays on the
/// song's grid - takes the chord sounding at that tick and plays one note of it for
/// `gate` of a step. Glides and lyrics belong to the notes the chord came from and are
/// dropped. A no-op for ArpMode::Off.
void arpeggiate(std::vector<ScheduledEvent>& events, const ArpSettings& arp);

/// Compiles placed breakpoints to a knot chain, curved segments subdivided until they
/// are within `tolerance` of the curve.
void compileBreakpoints(std::vector<Knot>& out, std::span<const AutomationPoint> points,
                        float tolerance);

/// The tolerance phase_4.md §4.0 asks for: 0.1 % of the parameter's range.
[[nodiscard]] float automationTolerance(float minimum, float maximum) noexcept;

/// Sort order of a channel's events: (tick, kind, noteId, instance), then endTick, so
/// a jump at a tick precedes the ramp that starts from it.
[[nodiscard]] bool eventLess(const ScheduledEvent& a, const ScheduledEvent& b) noexcept;

} // namespace adx::project
