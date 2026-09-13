// The realtime section: a span of execution bound by FINAL_PLAN §2.2 Rule 1.
//
// Entered once, at the top of the audio callback, by AudioThread::render. It is
// not a per-function concept - nothing below the callback enters or leaves it -
// and it nests, so a test may wrap a narrow piece of code in one without caring
// whether the caller already did.
#pragma once

#include "engine/rt/RtConfig.h"

namespace adx::rt {

#if ADX_ENABLE_RT_GUARD

namespace detail {

// Namespace-scope thread_local, not a function-local static, and deliberately so:
// inRtSection() is read inside operator new, on every allocation the process makes,
// so it has to compile to a bare TLS load. A function-local static thread_local
// adds an initialisation guard check to every access. Being thread_local, this is
// also not the kind of shared mutable state FINAL_PLAN §9 bans - there is one per
// thread and nothing synchronises on it. The t_ prefix marks thread-local storage.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
extern thread_local int t_rtDepth;

} // namespace detail

/// True when the calling thread is inside an RT section.
///
/// Cheap enough to call from operator new: one TLS load and a compare, no atomics.
[[nodiscard]] ADX_RT_HOT bool inRtSection() noexcept {
    return detail::t_rtDepth > 0;
}

/// Nesting depth, for tests that need to assert the section was actually entered.
[[nodiscard]] inline int rtSectionDepth() noexcept {
    return detail::t_rtDepth;
}

/// Marks the enclosing scope as realtime. Nests.
class ScopedRtSection {
public:
    ScopedRtSection() noexcept {
        ++detail::t_rtDepth;
    }
    ~ScopedRtSection() noexcept {
        --detail::t_rtDepth;
    }

    ScopedRtSection(const ScopedRtSection&) = delete;
    ScopedRtSection& operator=(const ScopedRtSection&) = delete;
    ScopedRtSection(ScopedRtSection&&) = delete;
    ScopedRtSection& operator=(ScopedRtSection&&) = delete;
};

#else

// Guard off (Release). Everything below folds away: inRtSection() is a compile-time
// false, so every `if (inRtSection())` in the allocator hook and RtCheckedMutex
// becomes dead code the optimizer deletes, and the RAII type has an empty body.
//
// This is why a clean Release run proves nothing about realtime safety, and why
// the positive-control test prints a notice instead of silently passing there.

[[nodiscard]] constexpr bool inRtSection() noexcept {
    return false;
}

[[nodiscard]] constexpr int rtSectionDepth() noexcept {
    return 0;
}

class ScopedRtSection {
public:
    // Empty, but user-provided rather than defaulted. A defaulted constructor and
    // destructor make the type trivial, and MSVC then reports every `ScopedRtSection
    // guard;` in the project as C4101 unreferenced-local-variable - which /WX turns
    // into a Release-only build failure. Writing the empty bodies out costs nothing
    // at runtime and keeps the call sites identical in both configurations.
    ScopedRtSection() noexcept {}
    ~ScopedRtSection() noexcept {}

    ScopedRtSection(const ScopedRtSection&) = delete;
    ScopedRtSection& operator=(const ScopedRtSection&) = delete;
    ScopedRtSection(ScopedRtSection&&) = delete;
    ScopedRtSection& operator=(ScopedRtSection&&) = delete;
};

#endif // ADX_ENABLE_RT_GUARD

} // namespace adx::rt
