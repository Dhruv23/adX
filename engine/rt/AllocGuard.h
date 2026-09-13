// The allocator hook: the detector that makes every other realtime test mean
// something.
//
// FINAL_PLAN §3.3.1 names an allocating audio callback as the worst defect in the
// archived engine, and the reason it survived is that nothing could detect it. This
// is the detector. AllocGuard.cpp replaces the global operator new / operator delete
// family; when the calling thread is inside an RT section, the allocation is
// recorded in the ViolationLog and then performed anyway.
//
// Performed anyway, deliberately: a hook that failed the allocation would turn a
// test failure into a crash and would change the behaviour of the code under test.
// The job is to observe.
#pragma once

#include "engine/rt/RtConfig.h"

namespace adx::rt {

/// True when the replacement operators in AllocGuard.cpp are actually part of this
/// binary.
///
/// Compiling the file is not enough. The linker pulls an object file out of a static
/// library only when something references a symbol in it, and the CRT already
/// supplies operator new - so without a reference, adx_engine_static.lib would
/// contain a hook that never gets linked and the whole suite would pass while
/// detecting nothing. Everything that depends on the hook calls this, and the
/// positive-control test asserts on it first.
[[nodiscard]] bool allocGuardLinked() noexcept;

/// Whether the hook was compiled in at all (ADX_ENABLE_RT_GUARD).
///
/// False in Release, where the guard is deliberately absent. A Release run that
/// reports zero violations has proven nothing, which is why the positive control
/// reports a skip there instead of passing.
[[nodiscard]] constexpr bool allocGuardCompiledIn() noexcept {
#if ADX_ENABLE_RT_GUARD
    return true;
#else
    return false;
#endif
}

/// Installs every runtime guard this phase provides: the assertion handler that
/// records instead of aborting inside a callback, and a reference that forces the
/// allocator hook to be linked.
///
/// Called by AudioThread's constructor, and directly by tests.
void installRtGuards() noexcept;

} // namespace adx::rt
