// One independent playback position.
//
// FINAL_PLAN §7's Phase 3 checkpoint: time is a *set* of sources, not a scalar. The
// arrangement playhead is source 0 and, until Phase 11, the only one anybody
// instantiates - but everything that schedules takes a `const TimeSource&` as a
// parameter, so a second source is a second array element rather than a transport
// rewrite. The archived engine's `double m_currentSamplePosition`, read directly at
// fourteen sites, is the thing this replaces (phase_3.md §2).
//
// Threading: everything here is called on the audio thread, except the published
// readouts at the bottom, which any thread may poll. Control changes arrive through
// the render engine's message queue and are *requested* when that queue is drained,
// then *applied* by beginBlock() - so a change always lands on a block boundary, and a
// test can drive a TimeSource directly with no queue at all.
#pragma once

#include <atomic>
#include <cstdint>

#include "engine/core/TempoMath.h"
#include "engine/core/Time.h"
#include "engine/transport/LoopRegion.h"
#include "engine/transport/PlayState.h"

namespace adx::transport {

struct TimeSourceId {
    std::uint32_t v{0};

    [[nodiscard]] friend constexpr bool operator==(TimeSourceId, TimeSourceId) noexcept = default;
};

/// Returned by TransportSet::acquire() when every source is in use.
inline constexpr TimeSourceId kInvalidTimeSource{0xFFFFFFFFU};

/// The Phase 11 clip grid's ceiling. Preallocated, so acquiring a source never
/// allocates - Phase 11 launches clips from the audio thread.
inline constexpr std::uint32_t kMaxTimeSources = 256;

/// What happened to a source during the block that just began. Consumed by the
/// scheduler to decide what to tell the voices (phase_3.md §4.11).
struct BlockTransition {
    /// A seek was applied at the top of this block.
    bool seeked{false};
    /// The source was rolling at the end of the previous block and is not now.
    bool stopped{false};
    /// The source was not rolling and now is.
    bool started{false};
    /// Rolling before the seek. Seek-while-stopped skips the voice release.
    bool wasRollingBeforeSeek{false};
};

class TimeSource {
public:
    TimeSource() noexcept;

    // --- binding (audio thread, when a snapshot is swapped in) ---

    /// Points this source at a tempo map, borrowed from the render snapshot and never
    /// owned. When the map actually differs from the current one, the *musical*
    /// position is kept - bar 17 stays bar 17 across a tempo edit - and the event
    /// cursors are invalidated, because every event's sample position just moved.
    void bindTempo(core::TempoView tempo, std::uint32_t sampleRate) noexcept;

    // --- control (audio thread, applied at the next beginBlock) ---

    void requestSeek(core::Ticks at) noexcept;
    void requestState(PlayState state) noexcept;
    void requestLoop(LoopRegion loop) noexcept;
    /// 1.0 is normal speed. Unused until Phase 11's tempo nudge; present now because
    /// adding a multiplier to an established position-advance later means auditing
    /// every call site.
    void requestRate(double rate) noexcept;

    /// Applies whatever was requested and reports what that did.
    BlockTransition beginBlock() noexcept;

    // --- the block (audio thread) ---

    /// Frames until the next point at which this source's block must be split - a
    /// loop wrap or a tempo change - capped at `frames`. Never zero for a non-zero
    /// `frames`: a boundary exactly at the current position has already been handled.
    [[nodiscard]] std::uint32_t framesToNextBoundary(std::uint32_t frames) const noexcept;

    /// The only call that moves the position. Returns true when this advance landed on
    /// the loop end and wrapped. Callers must not advance past a boundary reported by
    /// framesToNextBoundary - that is what makes the wrap land on an exact sample.
    bool advance(std::uint32_t frames) noexcept;

    // --- queries (audio thread) ---

    [[nodiscard]] std::int64_t positionSamples() const noexcept {
        return m_positionSamples;
    }
    /// Derived from samples through the tempo map, never stored: two stored
    /// representations of one position drift (phase_3.md §4.1).
    [[nodiscard]] core::Ticks positionTicks() const noexcept;
    [[nodiscard]] PlayState state() const noexcept {
        return m_state;
    }
    [[nodiscard]] bool rolling() const noexcept {
        return isRolling(m_state);
    }
    [[nodiscard]] const LoopRegion& loop() const noexcept {
        return m_loop;
    }
    [[nodiscard]] double rate() const noexcept {
        return m_rate;
    }
    [[nodiscard]] std::uint32_t sampleRate() const noexcept {
        return m_sampleRate;
    }
    [[nodiscard]] const core::TempoView& tempo() const noexcept {
        return m_tempo;
    }

    /// Where a tick lands on this source's own sample timeline.
    [[nodiscard]] std::int64_t samplesAt(core::Ticks at) const noexcept {
        return m_tempo.toSamples(at, m_sampleRate).value;
    }

    /// Loop bounds on the sample timeline. Meaningful only while loop().active().
    [[nodiscard]] std::int64_t loopStartSamples() const noexcept {
        return m_loopStartSamples;
    }
    [[nodiscard]] std::int64_t loopEndSamples() const noexcept {
        return m_loopEndSamples;
    }

    /// Bumped on every seek and every tempo rebind. An event cursor that remembers an
    /// older value re-searches; one that matches trusts its position. This is what
    /// lets cursor invalidation be correct without the scheduler knowing *why* the
    /// position jumped (phase_3.md §4.1).
    [[nodiscard]] std::uint64_t seekGeneration() const noexcept {
        return m_seekGeneration;
    }
    /// Bumped whenever the loop region's sample bounds change, so a cached "where the
    /// loop starts" cursor knows to re-search once - at the change, never at a wrap.
    [[nodiscard]] std::uint64_t loopGeneration() const noexcept {
        return m_loopGeneration;
    }

    // --- published readout (written by the audio thread once per block) ---

    /// Stores the position and state where any thread can read them. One call per
    /// block, at the end, so a reader sees the position the block finished at.
    void publish() noexcept;

    /// Any thread. The O(1) read a 60 Hz UI timer polls (FINAL_PLAN §2.2 Rule 2).
    [[nodiscard]] std::int64_t publishedTicks() const noexcept {
        return m_publishedTicks.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::int64_t publishedSamples() const noexcept {
        return m_publishedSamples.load(std::memory_order_acquire);
    }
    [[nodiscard]] PlayState publishedState() const noexcept {
        return static_cast<PlayState>(m_publishedState.load(std::memory_order_acquire));
    }

    /// Back to power-on state: stopped at zero, no loop, default tempo. Main thread,
    /// only while no audio thread is running this source.
    void reset() noexcept;

private:
    void recomputeLoopSamples() noexcept;
    void recomputeNextTempoChange() noexcept;

    core::TempoView m_tempo;
    std::uint32_t m_sampleRate{48000};

    std::int64_t m_positionSamples{0};
    /// Sub-sample remainder, for rates other than 1.0. At rate 1.0 it stays exactly
    /// zero, which is what keeps the normal path bit-exact.
    double m_fractionalSample{0.0};
    PlayState m_state{PlayState::Stopped};
    LoopRegion m_loop;
    double m_rate{1.0};

    std::int64_t m_loopStartSamples{0};
    std::int64_t m_loopEndSamples{0};
    /// The next tempo event's sample position, so a block splits at a tempo change
    /// without a search per block. INT64_MAX when there is none ahead.
    std::int64_t m_nextTempoChangeSamples{0};

    std::uint64_t m_seekGeneration{1};
    std::uint64_t m_loopGeneration{1};

    // Pending requests, applied by beginBlock().
    bool m_hasPendingSeek{false};
    core::Ticks m_pendingSeek;
    bool m_hasPendingState{false};
    PlayState m_pendingState{PlayState::Stopped};
    bool m_hasPendingLoop{false};
    LoopRegion m_pendingLoop;
    bool m_hasPendingRate{false};
    double m_pendingRate{1.0};

    bool m_wasRolling{false};

    std::atomic<std::int64_t> m_publishedTicks{0};
    std::atomic<std::int64_t> m_publishedSamples{0};
    std::atomic<std::uint8_t> m_publishedState{0};
};

} // namespace adx::transport
