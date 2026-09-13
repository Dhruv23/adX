// The mutex every part of the engine uses, and the runtime half of "no locks on the
// audio thread".
//
// clang-tidy bans <mutex> under the realtime paths, which catches the honest
// mistakes. It does not catch a lock taken inside a non-RT helper that the audio
// thread happens to call, and it cannot see into third-party code at all. So the
// mutex itself reports.
//
// std::mutex appearing anywhere in engine/ other than this file is a clang-tidy
// error, which is what makes "every engine mutex is an RtCheckedMutex" true rather
// than aspirational.
#pragma once

// The one deliberate <mutex> in the engine. Banning the header is how the realtime
// ban list bans the type, and this file is the wrapper that ban exists to funnel
// everyone into.
// NOLINTNEXTLINE(portability-restrict-system-includes)
#include <mutex>

#include "engine/rt/RtConfig.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"

namespace adx::rt {

/// A std::mutex that records a violation when locked from inside an RT section.
///
/// It still locks. A guard that refused would turn a latent priority inversion into
/// an immediate deadlock or data race, and the job here is to observe, not to
/// prevent - the same reasoning as the allocator hook.
class RtCheckedMutex {
public:
    RtCheckedMutex() noexcept = default;
    ~RtCheckedMutex() = default;

    RtCheckedMutex(const RtCheckedMutex&) = delete;
    RtCheckedMutex& operator=(const RtCheckedMutex&) = delete;
    RtCheckedMutex(RtCheckedMutex&&) = delete;
    RtCheckedMutex& operator=(RtCheckedMutex&&) = delete;

    void lock() noexcept {
        if (inRtSection()) {
            ADX_RECORD_VIOLATION(ViolationKind::Lock, 0);
        }
        m_impl.lock();
    }

    /// Always legal, even on the audio thread: it cannot block, so it cannot cause
    /// the inversion that makes lock() a violation. This is the escape hatch for the
    /// rare case where the audio thread genuinely wants to peek at shared state and
    /// is happy to be told no.
    [[nodiscard]] bool tryLock() noexcept {
        return m_impl.try_lock();
    }

    void unlock() noexcept {
        m_impl.unlock();
    }

private:
    std::mutex m_impl;
};

/// std::lock_guard equivalent over RtCheckedMutex, so callers never need <mutex>.
class RtLockGuard {
public:
    explicit RtLockGuard(RtCheckedMutex& mutex) noexcept : m_mutex(mutex) {
        m_mutex.lock();
    }
    ~RtLockGuard() {
        m_mutex.unlock();
    }

    RtLockGuard(const RtLockGuard&) = delete;
    RtLockGuard& operator=(const RtLockGuard&) = delete;
    RtLockGuard(RtLockGuard&&) = delete;
    RtLockGuard& operator=(RtLockGuard&&) = delete;

private:
    RtCheckedMutex& m_mutex;
};

} // namespace adx::rt
