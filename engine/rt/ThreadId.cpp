#include "engine/rt/ThreadId.h"

namespace adx::rt {
namespace {

// Thread-local, so "global mutable state" here is one bool per thread with no
// sharing and nothing to synchronise. FINAL_PLAN Â§9's ban exists to stop
// cross-cutting shared state; this is the opposite of that. The t_ prefix marks
// thread-local storage and is deliberate, so the naming check is waived with it.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
thread_local bool t_isAudioThread = false;

} // namespace

void registerAudioThread() noexcept {
    t_isAudioThread = true;
}

void unregisterAudioThread() noexcept {
    t_isAudioThread = false;
}

bool isAudioThread() noexcept {
    return t_isAudioThread;
}

} // namespace adx::rt
