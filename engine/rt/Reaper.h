// Deferred reclaim: the audio thread hands over objects it is done with, and the
// main thread destroys them.
//
// Iteration one had the right instinct - the audio thread must never call delete -
// and the wrong mechanism: it pushed retired snapshots into
// `AudioEngine::m_sequenceGarbageBin` (_archive/src-cpp/src/AudioEngine.cpp:145), a
// std::vector<unique_ptr<...>> mutated *from the audio thread*, so the act of
// avoiding a free introduced a push_back that can reallocate (FINAL_PLAN §3.2).
//
// Same idea, over a bounded lock-free ring.
#pragma once

#include <cstddef>
#include <type_traits>

#include "engine/rt/RtConfig.h"
#include "engine/rt/SpscRing.h"
#include "engine/rt/Violation.h"

namespace adx::rt {

/// One object waiting to be destroyed, as a pointer plus a type-erased deleter.
///
/// A raw function pointer, not std::function: std::function can allocate, and this
/// struct has to be trivially copyable to travel through the ring at all.
struct Retired {
    void* ptr{nullptr};
    void (*destroy)(void* ptr) noexcept {nullptr};
};

static_assert(std::is_trivially_copyable_v<Retired>);

/// Builds a Retired for a `T*` the caller is giving up ownership of.
///
/// The deleter is a stateless lambda per T, so the function pointer is resolved at
/// compile time and no type information travels at runtime.
template<class T> [[nodiscard]] Retired retireOf(T* ptr) noexcept {
    return Retired{.ptr = ptr, .destroy = [](void* raw) noexcept {
                       // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
                       // The one `delete` under engine/rt, and it is the point of the
                       // file: this lambda only ever runs inside Reaper::drain(), on
                       // the main thread. The realtime ban list cannot tell the two
                       // threads apart, so the exemption is stated here rather than
                       // by widening the ban.
                       delete static_cast<T*>(raw);
                   }};
}

class Reaper {
public:
    static constexpr std::size_t kCapacity = 1024;

    /// Audio thread. Queues `r` for destruction elsewhere.
    ///
    /// False means the ring is full, which means the main thread has stopped
    /// draining - a real fault, not backpressure to shrug at. It is recorded as a
    /// violation so the object does not leak in silence.
    [[nodiscard]] ADX_RT_HOT bool retire(Retired retired) noexcept {
        if (!m_queue.tryPush(retired)) {
            ADX_RECORD_VIOLATION(ViolationKind::Unbounded, 0);
            return false;
        }
        return true;
    }

    /// Main thread. Destroys everything queued and returns how many. Runs the
    /// deleters, so it must never be called from the audio thread.
    std::size_t drain() noexcept {
        std::size_t destroyed = 0;
        Retired retired{};
        while (m_queue.tryPop(retired)) {
            if (retired.destroy != nullptr && retired.ptr != nullptr) {
                retired.destroy(retired.ptr);
            }
            ++destroyed;
        }
        return destroyed;
    }

    [[nodiscard]] std::size_t pendingApprox() const noexcept {
        return m_queue.sizeApprox();
    }

private:
    SpscRing<Retired, kCapacity> m_queue;
};

} // namespace adx::rt
