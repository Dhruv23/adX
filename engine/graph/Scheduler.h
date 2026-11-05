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
#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>

#include "engine/graph/Audition.h"
#include "engine/graph/Crew.h"
#include "engine/graph/Node.h"
#include "engine/project/EventTrack.h"
#include "engine/project/Snapshot.h"
#include "engine/rt/BlockArena.h"
#include "engine/transport/TimeSource.h"
#include "engine/transport/TransportSet.h"

namespace adx::graph {

/// Time per graph step, for the optimization pass (bench/profile_nodes.py). Off unless
/// a profiling render switches it on; then each step's process() is timed and filed
/// under its position. One per process, written by the one thread rendering.
struct NodeProfile {
    static constexpr std::size_t kSteps = 4096;
    std::atomic<bool> enabled{false};
    std::array<std::string_view, kSteps> names{};
    std::array<std::uint64_t, kSteps> nanoseconds{};
    std::array<std::uint64_t, kSteps> calls{};
    std::size_t used{0};
};

[[nodiscard]] NodeProfile& nodeProfile() noexcept;

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
    /// Pieces run on the crew rather than walked in order on one thread.
    std::uint64_t parallelPieces{0};
};

/// A contiguous run of a track's events that fall in one sub-block.
struct EventRange {
    std::uint32_t begin{0};
    std::uint32_t end{0};
};

#if defined(_MSC_VER)
#    pragma warning(push)
// C4324: padded for the alignas below, which keeps the atomics on their own
// cache lines. Intended.
#    pragma warning(disable : 4324)
#endif

class Scheduler {
public:
    /// Renders `frames` into the interleaved `out`, which the caller has cleared.
    ///
    /// Applies every in-use source's pending requests at the top of the block, splits
    /// the block wherever any of them reaches a loop end or a tempo change, runs every
    /// step once per piece, and publishes each source's position at the end.
    /// `in` is the device input for the block, `inChannels` wide, or null; it reaches
    /// only the node of the track being monitored (Snapshot::monitorTable).
    void render(project::Snapshot& snapshot, transport::TransportSet& transport,
                rt::BlockArena& arena, float* out, std::uint32_t frames, std::uint32_t outChannels,
                const float* in = nullptr, std::uint32_t inChannels = 0) noexcept;

    /// Where the arrangement was at the start of the last block, after its requests
    /// applied, and whether it was rolling: where that block's input belongs.
    [[nodiscard]] std::int64_t blockStart() const noexcept {
        return m_blockStart;
    }
    [[nodiscard]] bool blockRolling() const noexcept {
        return m_blockRolling;
    }

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

    /// The audition and live-input events the next render() delivers to the steps of the
    /// channels they name (Audition.h). Each event's offset is from the block's start;
    /// it reaches the piece that contains it, at its offset within that piece, merged
    /// into the arrangement's events in order. Audio thread.
    void setAudition(std::span<const AuditionEvent> events) noexcept {
        m_audition = events;
    }

    /// The helpers a piece may be spread over, or null for one thread. Main thread,
    /// while no audio thread runs this scheduler. A piece runs on the crew when there is
    /// one with helpers, the graph carries a parallel plan, it is big enough to be worth
    /// the hand-offs, and the node profiler is off (it times steps one after another).
    /// Either way the output is the same, bit for bit (RenderGraph.h, ParallelPlan).
    void setCrew(Crew* crew) noexcept {
        m_crew = crew;
    }

    /// Below this many steps a piece is walked on one thread: the hand-offs would cost
    /// more than the work they spread.
    static constexpr std::size_t kParallelMinSteps = 16;

private:
    void runPiece(project::Snapshot& snapshot, transport::TransportSet& transport,
                  rt::BlockArena& arena, float* out, std::uint32_t offset, std::uint32_t frames,
                  std::uint32_t outChannels) noexcept;
    void runPieceParallel(project::Snapshot& snapshot, transport::TransportSet& transport,
                          rt::BlockArena& arena, float* out, std::uint32_t offset,
                          std::uint32_t frames, std::uint32_t outChannels) noexcept;
    /// The crew's job for one piece: take ready steps until none are left.
    static void parallelTask(void* user, std::uint32_t lane) noexcept;
    void runParallelStep(std::uint32_t stepIndex, std::uint32_t lane) noexcept;
    void pushReady(const ParallelPlan& plan, std::uint32_t step) noexcept;
    [[nodiscard]] bool popReady(const ParallelPlan& plan, std::uint32_t& step) noexcept;

    const float* m_input{nullptr};
    std::uint32_t m_inputChannels{0};
    std::int64_t m_blockStart{0};
    bool m_blockRolling{false};
    static void applyAutomation(project::Snapshot& snapshot,
                                const transport::TimeSource& arrangement) noexcept;
    /// The per-frame values of the automated parameters among a step's, from the
    /// per-callback arena; empty when none of them is automated.
    [[nodiscard]] static std::span<const float* const>
    automationFor(const NodeStep& step, project::Snapshot& snapshot,
                  const transport::TimeSource& arrangement, rt::BlockArena& arena,
                  std::uint32_t frames) noexcept;
    /// Reads the scheduler's per-block state but writes only `stats` and the step's
    /// own cursor, so steps on different threads may call it at once.
    [[nodiscard]] EventView eventsFor(const NodeStep& step, project::Snapshot& snapshot,
                                      transport::TransportSet& transport, rt::BlockArena& arena,
                                      std::uint32_t frames, SchedulerStats& stats) noexcept;

    // advanceCursor and fillEvents, counting into `stats`: each lane has its own.
    [[nodiscard]] static EventRange advance(SchedulerStats& stats,
                                            const transport::TimeSource& time,
                                            const project::EventTrack& track,
                                            project::EventCursor& cursor, std::uint32_t frames,
                                            bool wrapped) noexcept;
    static std::uint32_t fill(SchedulerStats& stats, const transport::TimeSource& time,
                              std::uint32_t timeSource, const project::EventTrack& track,
                              EventRange range, std::span<BlockEvent> out,
                              std::uint32_t first) noexcept;
    [[nodiscard]] static std::uint32_t lowerBound(SchedulerStats& stats,
                                                  const transport::TimeSource& time,
                                                  const project::EventTrack& track,
                                                  std::int64_t samples) noexcept;

    SchedulerStats m_stats;
    /// What each crew lane counted during the piece being run, added into m_stats after.
    std::array<SchedulerStats, Crew::kMaxLanes> m_laneStats{};
    Crew* m_crew{nullptr};

    // The piece being run on the crew, for every lane to read. Written before
    // Crew::run publishes the job, read only inside it.
    project::Snapshot* m_pieceSnapshot{nullptr};
    transport::TransportSet* m_pieceTransport{nullptr};
    rt::BlockArena* m_pieceArena{nullptr};
    std::uint32_t m_pieceOffset{0};
    std::uint32_t m_pieceFrames{0};
    // The ready queue's ends and the steps not yet finished, on their own cache lines:
    // every lane hits them.
    alignas(64) std::atomic<std::uint32_t> m_readyHead{0};
    alignas(64) std::atomic<std::uint32_t> m_readyTail{0};
    alignas(64) std::atomic<std::uint32_t> m_remaining{0};

    /// Per source, carried from the top of a block (or the end of the previous piece)
    /// to the first piece that can act on it.
    std::array<bool, transport::kMaxTimeSources> m_releasePending{};
    std::array<bool, transport::kMaxTimeSources> m_wrapPending{};
    std::span<const AuditionEvent> m_audition;
    /// Where the piece being run starts in its block, for placing audition events.
    std::uint32_t m_pieceStart{0};
};

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

} // namespace adx::graph
