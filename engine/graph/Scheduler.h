// Event dispatch and node execution: one audio block, from a snapshot and a set of
// time sources.
//
// This is where FINAL_PLAN §3.3 items 2 and 3 are fixed. Iteration one, per callback,
// built a vector of events (an allocation), scanned every note in the project to find
// the ones starting in the block, then rescanned that vector once per frame. Here:
//
//   - each track keeps a cursor, re-searched only when its source's seek generation
//     changes, so a block's event work is proportional to the events *in* it;
//   - a loop wrap moves the cursor to a cached loop-start index - no search at all;
//   - events reach a node already converted to offsets and sorted, and the node
//     consumes them with one advancing index.
//
// And the Phase 11 checkpoint is here in its structural form: render() takes the set
// of sources, every step names the source that drives it, and each node is handed
// *its* source in ProcessContext. There is no global position anywhere below this.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/graph/Node.h"
#include "engine/project/EventTrack.h"
#include "engine/project/Snapshot.h"
#include "engine/rt/BlockArena.h"
#include "engine/transport/TimeSource.h"
#include "engine/transport/TransportSet.h"

namespace adx::graph {

/// Work counters. Not atomics: the audio thread writes them and a test reads them
/// after the stream has stopped. These are what make "amortised" an assertion rather
/// than a claim (scheduler_cursor_is_amortized, scheduler_no_rescan_on_loop_wrap).
struct SchedulerStats {
    std::uint64_t blocks{0};
    std::uint64_t subBlocks{0};
    /// Binary searches for a cursor position: a seek, a tempo rebind, a new loop, a
    /// fresh snapshot. Never a wrap.
    std::uint64_t cursorSearches{0};
    /// Tick-to-sample conversions performed by those searches.
    std::uint64_t searchProbes{0};
    /// Tick-to-sample conversions performed walking a cursor forward.
    std::uint64_t eventComparisons{0};
    std::uint64_t eventsDispatched{0};
};

/// A contiguous run of a track's events that fall in one sub-block.
struct EventRange {
    std::uint32_t begin{0};
    std::uint32_t end{0};
};

class Scheduler {
public:
    /// Renders `frames` into the interleaved `out`, which the caller has cleared.
    ///
    /// Applies every in-use source's pending requests at the top of the block, splits
    /// the block wherever any of them reaches a loop end or a tempo change, runs every
    /// step once per piece, and publishes each source's position at the end.
    void render(project::Snapshot& snapshot, transport::TransportSet& transport,
                rt::BlockArena& arena, float* out, std::uint32_t frames,
                std::uint32_t outChannels) noexcept;

    /// Positions `cursor` for `time` and walks it across the next `frames`, returning
    /// the events that fall inside. `wrapped` means the source wrapped its loop at the
    /// end of the previous piece, so the walk restarts from the cached loop index.
    ///
    /// Public because the checkpoint test drives two sources through one scheduler
    /// directly, and because it is the unit the amortisation counters measure.
    [[nodiscard]] EventRange advanceCursor(const transport::TimeSource& time,
                                           const project::EventTrack& track,
                                           project::EventCursor& cursor, std::uint32_t frames,
                                           bool wrapped) noexcept;

    /// Converts `range` to offsets from the source's current position and appends them
    /// to `out` from index `first`. Returns the index after the last written.
    std::uint32_t fillEvents(const transport::TimeSource& time, std::uint32_t timeSource,
                             const project::EventTrack& track, EventRange range,
                             std::span<BlockEvent> out, std::uint32_t first) noexcept;

    [[nodiscard]] const SchedulerStats& stats() const noexcept {
        return m_stats;
    }
    void resetStats() noexcept {
        m_stats = SchedulerStats{};
    }

    /// Forgets per-source carry-over (a pending release or wrap). Main thread, while no
    /// audio thread is running this scheduler.
    void reset() noexcept;

private:
    void runPiece(project::Snapshot& snapshot, transport::TransportSet& transport,
                  rt::BlockArena& arena, float* out, std::uint32_t offset, std::uint32_t frames,
                  std::uint32_t outChannels) noexcept;
    static void applyAutomation(project::Snapshot& snapshot,
                                const transport::TimeSource& arrangement) noexcept;
    /// The per-frame values of the automated parameters among a step's, from the
    /// per-callback arena; empty when none of them is automated.
    [[nodiscard]] static std::span<const float* const>
    automationFor(const NodeStep& step, project::Snapshot& snapshot,
                  const transport::TimeSource& arrangement, rt::BlockArena& arena,
                  std::uint32_t frames) noexcept;
    [[nodiscard]] EventView eventsFor(const NodeStep& step, project::Snapshot& snapshot,
                                      transport::TransportSet& transport, rt::BlockArena& arena,
                                      std::uint32_t frames) noexcept;

    [[nodiscard]] std::uint32_t lowerBound(const transport::TimeSource& time,
                                           const project::EventTrack& track,
                                           std::int64_t samples) noexcept;

    SchedulerStats m_stats;

    /// Per source, carried from the top of a block (or the end of the previous piece)
    /// to the first piece that can act on it.
    std::array<bool, transport::kMaxTimeSources> m_releasePending{};
    std::array<bool, transport::kMaxTimeSources> m_wrapPending{};
};

} // namespace adx::graph
