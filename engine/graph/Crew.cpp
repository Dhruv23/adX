// adx-thread: realtime
#include "engine/graph/Crew.h"

#include <algorithm>
#include <chrono>

#include "engine/rt/RtSection.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#    define ADX_CREW_SSE 1
#    include <emmintrin.h>
#    include <xmmintrin.h>
#else
#    define ADX_CREW_SSE 0
#endif

namespace adx::graph {
namespace {

/// How long a helper keeps checking for the next job before it sleeps. A block's
/// pieces run back to back, so a helper that just finished one usually sees the next
/// within microseconds; between blocks (5 ms at 256 frames) it sleeps rather than burn
/// a core.
constexpr std::chrono::microseconds kSpin{50};

} // namespace

void cpuRelax() noexcept {
#if ADX_CREW_SSE
    _mm_pause();
#endif
}

std::uint32_t readControlWord() noexcept {
#if ADX_CREW_SSE
    return _mm_getcsr();
#else
    return 0;
#endif
}

void writeControlWord(std::uint32_t word) noexcept {
#if ADX_CREW_SSE
    _mm_setcsr(word);
#else
    static_cast<void>(word);
#endif
}

void Crew::configure(std::uint32_t lanes, std::span<float> scratch,
                     std::span<rt::BlockArena> arenas) noexcept {
    const auto fit = static_cast<std::uint32_t>(
        std::min<std::size_t>({kMaxLanes, scratch.size() / kScratchFloats, arenas.size() + 1}));
    m_lanes = std::clamp(lanes, 1U, std::max(fit, 1U));
    m_scratch = scratch;
    m_arenas = arenas;
}

void Crew::run(Task task, void* user) noexcept {
    if (m_lanes <= 1) {
        task(user, 0);
        return;
    }
    // Every helper is out of the last job (run() waited for it), so nothing reads these
    // while they are written. A helper reads them only after it has seen m_open true,
    // which is stored after them.
    m_task = task;
    m_user = user;
    m_csr = readControlWord();
    m_open.store(true, std::memory_order_seq_cst);
    m_generation.fetch_add(1, std::memory_order_seq_cst);
    // A helper registers as a sleeper before its last look at the generation, so either
    // it sees the new one or this sees it asleep (seq_cst on both sides).
    if (m_sleepers.load(std::memory_order_seq_cst) > 0) {
        m_generation.notify_all();
    }

    task(user, 0);

    // Closed: a helper that gets in now leaves without touching the job. Then wait out
    // the ones already inside - they are finishing the last nodes, or finding none.
    m_open.store(false, std::memory_order_seq_cst);
    while (m_inFlight.load(std::memory_order_seq_cst) != 0) {
        cpuRelax();
    }
}

std::uint64_t Crew::helpOnce(std::uint32_t lane, std::uint64_t seen,
                             const std::atomic<bool>& stopping) noexcept {
    std::uint64_t now = m_generation.load(std::memory_order_acquire);
    if (now == seen) {
        const auto until = std::chrono::steady_clock::now() + kSpin;
        std::uint32_t spins = 0;
        while (now == seen) {
            cpuRelax();
            now = m_generation.load(std::memory_order_acquire);
            if ((++spins & 63U) == 0 && std::chrono::steady_clock::now() >= until) {
                break;
            }
        }
    }
    while (now == seen) {
        if (stopping.load(std::memory_order_acquire)) {
            return seen;
        }
        m_sleepers.fetch_add(1, std::memory_order_seq_cst);
        now = m_generation.load(std::memory_order_seq_cst);
        if (now == seen) {
            m_generation.wait(seen, std::memory_order_seq_cst);
        }
        m_sleepers.fetch_sub(1, std::memory_order_seq_cst);
        now = m_generation.load(std::memory_order_acquire);
    }

    m_inFlight.fetch_add(1, std::memory_order_seq_cst);
    if (m_open.load(std::memory_order_seq_cst)) {
        m_joins.fetch_add(1, std::memory_order_relaxed);
        if (readControlWord() != m_csr) {
            writeControlWord(m_csr);
        }
        const rt::ScopedRtSection realtime;
        m_task(m_user, lane);
    }
    m_inFlight.fetch_sub(1, std::memory_order_seq_cst);
    return now;
}

void Crew::wakeAll() noexcept {
    // A new generation with the job closed: a sleeper wakes, finds nothing to join, and
    // reads its stop flag. Bumping the value rather than only notifying means a helper
    // that was about to sleep cannot miss it.
    m_generation.fetch_add(1, std::memory_order_seq_cst);
    m_generation.notify_all();
}

} // namespace adx::graph
