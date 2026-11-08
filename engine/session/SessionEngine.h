// adx-thread: realtime
//
// The session on the audio thread (phase_11.md §4.3-4.5): launch requests resolved on
// the musical grid at exact sample offsets, each launched clip on a time source of its
// own, and a clip that stops released rather than cut.
//
// The grid is the arrangement's while it rolls. While it does not, the session keeps a
// clock of its own that starts with the first launch, so a clip launched with nothing
// playing starts at once and the next one lands on that clip's bars.
//
// A clip's source reads the tempo map from where the clip was launched (a copy of the
// map shifted to that tick, kept in the clip's runtime), so a clip launched after a tempo
// change plays at the new tempo; and it runs at the master's rate, block by block, so a
// nudge, a tap or a scene's tempo moves every clip with the grid.
//
// One clip plays per track. Launching another in the same track stops the first at the
// same boundary - its lane is told the offset, so its later notes never play and its
// voices release there (SessionLane::stopAt) - while the new one starts on a fresh
// source. A stopped clip keeps its source until no voice of it sounds (the scheduler's
// per-source flags, set by the channels), so its releases run out under its own source
// id; then the source returns to the pool.
//
// The end of a pass is found on the grid before the block renders, so a follow action
// lands on the clip's last sample exactly, and every pass, stop and rate change can be
// logged at its tick for the performance recorder (PerformanceLog.h).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "engine/core/TempoMath.h"
#include "engine/graph/Scheduler.h"
#include "engine/project/Snapshot.h"
#include "engine/rt/OwnedArray.h"
#include "engine/session/ClipState.h"
#include "engine/session/PerformanceLog.h"
#include "engine/session/TempoNudge.h"
#include "engine/transport/TransportSet.h"

namespace adx::session {

/// A request from the main thread or a pad, carried in EngineMessage.
struct SessionRequest {
    enum class Kind : std::uint8_t {
        Launch,
        Stop,
        Scene,
        StopAll,
        /// `value` is the master rate (a nudge, or a tempo set from the transport bar).
        Rate,
        /// A tap of tap tempo; `value` is when, in seconds on any monotonic clock, or
        /// negative for "now" on the audio clock.
        Tap,
        /// `value` non-zero arms the performance log, zero disarms it.
        Record,
    } kind{Kind::Launch};
    std::uint32_t track{0};
    std::uint32_t row{0};
    /// Launch and Scene: the pad's gain (its velocity through the profile's curve),
    /// multiplying the cell's own; 0 means none (1).
    float gain{0.0F};
    double value{0.0};
};

class SessionEngine {
public:
    static constexpr std::size_t kMaxClips = 128;
    static constexpr std::size_t kMaxTracks = 64;
    static constexpr std::size_t kMaxRows = 64;
    /// The longest a stopped clip's source waits for its voices to fall silent: ten
    /// seconds. A voice still sounding then is a hung note, and is left to its channel.
    static constexpr std::uint64_t kMaxTailFrames = 480000;
    /// Tempo events a clip's shifted map keeps; later ones hold the last.
    static constexpr std::size_t kTempoEvents = 32;
    static constexpr std::size_t kLogCapacity = 8192;

    /// MAIN THREAD, before the stream opens: the clip runtimes, lanes and cell states,
    /// on the heap (an engine is lighter on a test's stack for it). Until then the
    /// session plays nothing and every cell reads stopped.
    void prepare();

    /// AUDIO THREAD. Queues a request; resolved at its boundary by the next blocks.
    void request(const SessionRequest& request) noexcept;

    /// AUDIO THREAD, when a snapshot is swapped in: every running clip is pointed at its
    /// cell's events in the new snapshot, or released if its cell is gone.
    void rebind(const project::Snapshot& snapshot, transport::TransportSet& transport) noexcept;

    /// AUDIO THREAD, before the render: resolves the requests and pass ends that fall in
    /// this block, starts and stops clips, and returns the lanes the scheduler is to play.
    [[nodiscard]] std::span<const graph::SessionLane>
    beforeRender(const project::Snapshot& snapshot, transport::TransportSet& transport,
                 std::uint32_t frames) noexcept;
    /// AUDIO THREAD, after the render: progress, and the stopped clips whose voices have
    /// all fallen silent (`sounding`, one flag per time source, read and cleared here).
    void afterRender(const project::Snapshot& snapshot, transport::TransportSet& transport,
                     std::uint32_t frames, std::span<std::atomic<std::uint8_t>> sounding) noexcept;

    // --- any thread ---

    /// A cell's state, as the session panel and the pad LEDs show it.
    [[nodiscard]] ClipState state(std::uint32_t track, std::uint32_t row) const noexcept;
    /// Where the playing clip of `track` is in its loop, 0..1; negative when none plays.
    [[nodiscard]] float progress(std::uint32_t track) const noexcept;
    /// Launches refused because every clip runtime or time source was in use.
    [[nodiscard]] std::uint64_t refused() const noexcept {
        return m_refused.load(std::memory_order_acquire);
    }
    /// Clips sounding or releasing now.
    [[nodiscard]] std::uint32_t active() const noexcept {
        return m_active.load(std::memory_order_acquire);
    }
    /// The master rate the session runs at (1 is the tempo map's own tempo).
    [[nodiscard]] double rate() const noexcept {
        return m_publishedRate.load(std::memory_order_acquire);
    }
    /// The tempo the last taps gave, in bpm; 0 before two taps.
    [[nodiscard]] double tappedBpm() const noexcept {
        return m_tapped.load(std::memory_order_acquire);
    }
    /// Whether the performance log is armed.
    [[nodiscard]] bool recording() const noexcept {
        return m_recordingPublished.load(std::memory_order_acquire);
    }

    /// MAIN THREAD: the next logged event, or false when none waits.
    [[nodiscard]] bool takeLogged(PerformanceEvent& out) noexcept;
    /// Events lost because the log was full.
    [[nodiscard]] std::uint64_t logLost() const noexcept {
        return m_logLost.load(std::memory_order_acquire);
    }

private:
    struct Runtime {
        bool used{false};
        bool releasing{false};
        std::uint32_t track{0};
        std::uint32_t row{0};
        transport::TimeSourceId source{transport::kInvalidTimeSource};
        project::EventCursor cursor{};
        graph::SessionLane lane{};
        /// Frames since the stop, while releasing.
        std::uint64_t tailFrames{0};
        std::uint32_t passes{0};
        bool loop{true};
        /// The stop offset was given to the lane this block; the source stops next block.
        bool stopIssued{false};
        /// The grid tick of this pass's clip tick 0, and the clip's loop length.
        std::int64_t originTick{0};
        std::int64_t length{1};
        float gain{1.0F};
        /// The tempo map from originTick on, shifted to start at 0: what the source reads.
        std::array<core::TempoEvent, kTempoEvents> tempoEvents{};
        std::array<double, kTempoEvents> tempoSeconds{};
        std::uint32_t tempoCount{0};
    };
    struct Pending {
        bool waiting{false};
        bool stop{false};
        std::uint32_t row{0};
        float gain{0.0F};
    };
    struct Grid {
        std::int64_t fromTicks{0};
        std::int64_t toTicks{0};
        std::int64_t fromSamples{0};
        double rate{1.0};
        bool valid{false};
    };

    [[nodiscard]] Grid grid(const project::Snapshot& snapshot,
                            const transport::TransportSet& transport,
                            std::uint32_t frames) const noexcept;
    /// The frame in this block the grid's `tick` falls on.
    [[nodiscard]] static std::uint32_t offsetOf(const project::Snapshot& snapshot, const Grid& grid,
                                                std::int64_t tick, std::uint32_t frames) noexcept;
    /// Starts `clip` at frame `offset`, grid tick `tick`, from its own tick `phase`.
    void start(const project::Snapshot& snapshot, transport::TransportSet& transport,
               const project::SessionClip& clip, std::uint32_t offset, std::int64_t tick,
               std::int64_t phase, float gain, std::uint32_t passes) noexcept;
    void stop(std::uint32_t track, std::uint32_t offset, std::int64_t tick) noexcept;
    /// Resolves what falls in this block for one track: its request, its pass ends.
    void resolveTrack(const project::Snapshot& snapshot, transport::TransportSet& transport,
                      const Grid& grid, std::uint32_t track, std::uint32_t frames) noexcept;
    /// Applies a pending request whose boundary is `tick` (frame `offset`).
    void applyPending(const project::Snapshot& snapshot, transport::TransportSet& transport,
                      std::uint32_t track, std::uint32_t offset, std::int64_t tick) noexcept;
    /// A pass of the track's clip ended at `tick`: loop, follow, or stop.
    void passEnded(const project::Snapshot& snapshot, transport::TransportSet& transport,
                   Runtime& runtime, std::uint32_t offset, std::int64_t tick) noexcept;
    static void shiftTempo(Runtime& runtime, const core::TempoView& map) noexcept;
    [[nodiscard]] static core::TempoView tempoOf(const Runtime& runtime) noexcept;
    void setRate(transport::TransportSet& transport, double rate, std::int64_t tick) noexcept;
    [[nodiscard]] double masterRate(const transport::TransportSet& transport) const noexcept;
    void log(const PerformanceEvent& event) noexcept;
    [[nodiscard]] Runtime* playing(std::uint32_t track) noexcept;
    void setState(std::uint32_t track, std::uint32_t row, ClipState state) noexcept;
    [[nodiscard]] ClipState rawState(std::uint32_t track, std::uint32_t row) const noexcept;

    rt::OwnedArray<Runtime> m_runtime;
    std::array<Pending, kMaxTracks> m_pending{};
    rt::OwnedArray<graph::SessionLane> m_lanes;
    rt::OwnedArray<std::atomic<std::uint8_t>> m_states;
    std::array<std::atomic<float>, kMaxTracks> m_progress{};
    std::atomic<std::uint64_t> m_refused{0};
    std::atomic<std::uint32_t> m_active{0};
    /// The session's own clock while the arrangement is stopped, in samples of the
    /// tempo map; and whether it runs (a clip plays, a launch waits, or a recording).
    double m_clock{0.0};
    bool m_clockRunning{false};
    /// A scene row to fire, expanded per track at the next block; none when all ones.
    std::uint32_t m_scene{0xFFFFFFFFU};
    float m_sceneGain{0.0F};
    /// A fired scene's tempo, taken over at the global quantize's next boundary.
    double m_sceneBpm{0.0};
    /// The master rate: the session clock's, and the arrangement's once set here.
    double m_rate{1.0};
    std::atomic<double> m_publishedRate{1.0};
    /// A rate asked for (a nudge, a tap), applied at the next block.
    double m_pendingRate{0.0};
    TapTempo m_tap;
    std::atomic<double> m_tapped{0.0};
    /// Frames rendered, for a tap that says "now", at the snapshot's rate.
    std::uint64_t m_frames{0};
    std::uint32_t m_sampleRate{48000};
    bool m_recording{false};
    std::atomic<bool> m_recordingPublished{false};
    /// The log: a single-producer ring on the heap (it is 400 KB), written here and read
    /// by takeLogged on the main thread.
    rt::OwnedArray<PerformanceEvent> m_log;
    std::atomic<std::size_t> m_logHead{0};
    std::atomic<std::size_t> m_logTail{0};
    std::atomic<std::uint64_t> m_logLost{0};
};

} // namespace adx::session
