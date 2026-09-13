// Project-wide compile-time machinery: assertions, the unreachable marker, and
// forced inlining.
//
// This header includes nothing, deliberately. It is included by realtime code,
// and the realtime ban list (.clang-tidy-rt) bans a type by banning the header it
// lives in - which applies transitively, so anything this file pulls in becomes
// unbannable everywhere below the audio callback. The default assertion handler
// needs <cstdio>; that is why it lives in Config.cpp and this file only declares
// it (FINAL_PLAN §9: headers declare, implementation files implement).
#pragma once

// Defined by engine/CMakeLists.txt in Debug and RelWithDebInfo, absent in
// Release. Keyed off an adX macro rather than NDEBUG because CMake defines NDEBUG
// in RelWithDebInfo, which is exactly the config where assertions must stay
// (phase_0.md §4.3).
#if !defined(ADX_ASSERTIONS_ENABLED)
#    define ADX_ASSERTIONS_ENABLED 0
#endif

#if defined(_MSC_VER)
#    define ADX_FORCE_INLINE __forceinline
#    define ADX_DEBUG_BREAK() __debugbreak()
#    define ADX_UNREACHABLE() __assume(false)
#else
#    define ADX_FORCE_INLINE inline __attribute__((always_inline))
#    define ADX_DEBUG_BREAK() __builtin_trap()
#    define ADX_UNREACHABLE() __builtin_unreachable()
#endif

namespace adx::core {

/// What a failed ADX_ASSERT does. Must be noexcept: assertions fire in realtime
/// paths, and RT code never throws (phase_0.md §4.2).
using AssertHandler = void (*)(const char* expression, const char* file, int line) noexcept;

/// The default handler: complain to stderr and stop. Correct on the main thread,
/// wrong on the audio thread, which is the entire reason the handler is
/// replaceable.
[[noreturn]] void abortOnAssertFailure(const char* expression, const char* file, int line) noexcept;

/// Installs `handler` and returns the one it replaced. nullptr restores the
/// default. Main thread only, and in practice only at startup.
///
/// Phase 1 installs a handler that records a realtime violation and *returns*
/// instead of aborting, so a failed assertion inside an audio callback fails the
/// test suite rather than killing the process mid-buffer. That requirement is why
/// ADX_ASSERT is not plain assert() (phase_0.md §4.3, §8).
AssertHandler setAssertHandler(AssertHandler handler) noexcept;

/// Routes a failure to the installed handler. ADX_ASSERT expands to this; call it
/// directly only when writing a new assertion macro.
void reportAssertFailure(const char* expression, const char* file, int line) noexcept;

} // namespace adx::core

#if ADX_ASSERTIONS_ENABLED
/// Checks an invariant. Routed through adx::core::setAssertHandler, so a failure
/// inside an audio callback records a violation rather than aborting the process.
#    define ADX_ASSERT(condition)                                                                  \
        do {                                                                                       \
            if (!(condition)) {                                                                    \
                ::adx::core::reportAssertFailure(#condition, __FILE__, __LINE__);                  \
            }                                                                                      \
        } while (false)
#else
// Compiled out, but the condition still has to parse and name real things, so a
// Release-only build cannot rot an assertion Debug builds rely on. sizeof leaves
// the expression unevaluated, which also keeps /W4 from reporting the variables it
// mentions as unused.
#    define ADX_ASSERT(condition) ((void)sizeof(!(condition)))
#endif
