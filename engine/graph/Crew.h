// adx-thread: realtime
//
// The helper threads a render can spread one piece of a block over, as the audio
// thread sees them.
//
// The design is Ardour's process graph, cut down to what a fixed topological plan
// needs: the thread that owns the callback runs the work itself, and helpers that are
// awake join in. Nothing here blocks the audio thread: it publishes a job, wakes
// sleeping helpers only if there are any, works through the job as lane 0, and then
// waits - spinning - only for helpers that are still inside the job. A helper that
// wakes late finds the job closed and goes back to sleep; it never touches a job that
// has finished, because the audio thread does not return until every helper that got
// in has got out (the in-flight count below, a Dekker handshake on seq_cst atomics).
//
// Helpers inherit the audio thread's floating-point control word for every job. FTZ
// and DAZ change results, and MXCSR is per thread, so a helper that flushed denormals
// differently from the audio thread would make the output depend on which thread ran
// which node (Denormal.h). They also run inside an RT section, so the allocator guard
// sees their allocations exactly as it sees the audio thread's.
//
// The threads themselves - creation, priority, shutdown - are main-thread code in
// engine/render/RenderWorkers.cpp, where <thread> may be included.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "engine/rt/BlockArena.h"
#include "engine/rt/RtConfig.h"

namespace adx::graph {

#if defined(_MSC_VER)
#    pragma warning(push)
// C4324: padded for the alignas below, which keeps the atomics on their own
// cache lines. Intended.
#    pragma warning(disable : 4324)
#endif

class Crew {
public:
    /// Lane 0 is the thread that calls run(); lanes 1.. are helpers.
    static constexpr std::uint32_t kMaxLanes = 16;
    /// Floats of accumulator scratch per lane: every input port, both channels, a full
    /// block.
    static constexpr std::size_t kScratchFloats = std::size_t{4} * rt::kMaxBlockFrames;

    /// A job: called once on every lane that joins it. It returns when there is no work
    /// left for it to do.
    using Task = void (*)(void* user, std::uint32_t lane) noexcept;

    Crew() noexcept = default;
    Crew(const Crew&) = delete;
    Crew& operator=(const Crew&) = delete;
    Crew(Crew&&) = delete;
    Crew& operator=(Crew&&) = delete;
    ~Crew() = default;

    /// Main thread, before any run(): how many lanes, and each lane's scratch and
    /// arena. Lane 0's arena is the caller's own and is not set here.
    void configure(std::uint32_t lanes, std::span<float> scratch,
                   std::span<rt::BlockArena> arenas) noexcept;

    [[nodiscard]] std::uint32_t lanes() const noexcept {
        return m_lanes;
    }
    /// Lane `lane`'s accumulator scratch, kScratchFloats long.
    [[nodiscard]] std::span<float> scratch(std::uint32_t lane) const noexcept {
        return m_scratch.subspan(static_cast<std::size_t>(lane) * kScratchFloats, kScratchFloats);
    }
    /// A helper lane's arena (lane >= 1).
    [[nodiscard]] rt::BlockArena& arena(std::uint32_t lane) const noexcept {
        return m_arenas[lane - 1];
    }

    /// AUDIO THREAD. Runs `task` here as lane 0 and on every helper that joins, and
    /// returns once lane 0's call has returned and no helper is still inside it.
    void run(Task task, void* user) noexcept;

    // --- helper side (RenderWorkers' threads) --------------------------------------

    /// Waits for a job newer than `seen` - spinning for a short while, then sleeping -
    /// and runs it as `lane`. Returns the job's generation, or `seen` when woken to
    /// stop. `stopping` is read each time the helper wakes.
    std::uint64_t helpOnce(std::uint32_t lane, std::uint64_t seen,
                           const std::atomic<bool>& stopping) noexcept;

    /// Main thread: wakes every sleeping helper, so one that is stopping notices.
    void wakeAll() noexcept;

    /// Main thread, for tests: jobs run, and how many lanes joined them.
    [[nodiscard]] std::uint64_t jobs() const noexcept {
        return m_generation.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t helperJoins() const noexcept {
        return m_joins.load(std::memory_order_relaxed);
    }

private:
    std::uint32_t m_lanes{1};
    std::span<float> m_scratch;
    std::span<rt::BlockArena> m_arenas;

    // Written by the audio thread before m_generation moves; read by helpers after.
    Task m_task{nullptr};
    void* m_user{nullptr};
    std::uint32_t m_csr{0};

    // On their own cache lines: the audio thread and every helper hit these.
    alignas(64) std::atomic<std::uint64_t> m_generation{0};
    alignas(64) std::atomic<bool> m_open{false};
    alignas(64) std::atomic<std::uint32_t> m_inFlight{0};
    alignas(64) std::atomic<std::uint32_t> m_sleepers{0};
    alignas(64) std::atomic<std::uint64_t> m_joins{0};
};

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

/// One spin-wait iteration: a pause where the CPU has one.
void cpuRelax() noexcept;

/// The calling thread's floating-point control word, and setting it. 0 / no-op where
/// there is no SSE.
[[nodiscard]] std::uint32_t readControlWord() noexcept;
void writeControlWord(std::uint32_t word) noexcept;

} // namespace adx::graph
