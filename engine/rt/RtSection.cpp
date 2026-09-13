#include "engine/rt/RtSection.h"

#if ADX_ENABLE_RT_GUARD

namespace adx::rt::detail {

// One definition, in one translation unit, so operator new in AllocGuard.cpp and
// every ScopedRtSection in the program agree on the same counter. See the
// declaration in RtSection.h for why this is namespace-scope rather than a
// function-local static.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
thread_local int t_rtDepth = 0;

} // namespace adx::rt::detail

#endif
