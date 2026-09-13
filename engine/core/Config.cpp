#include "engine/core/Config.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace adx::core {
namespace {

/// The one piece of mutable process state in the engine.
///
/// FINAL_PLAN §9 bans module-level mutable globals and clang-tidy enforces it; a
/// function-local static is the exception this needs, because ADX_ASSERT has to be
/// routable at runtime and there is exactly one assertion handler per process.
/// Initialised from the address of a function, so it is constant-initialised and
/// there is no static-initialisation race to lose.
std::atomic<AssertHandler>& handlerSlot() noexcept {
    static std::atomic<AssertHandler> slot{&abortOnAssertFailure};
    return slot;
}

} // namespace

void abortOnAssertFailure(const char* expression, const char* file, int line) noexcept {
    std::fprintf(stderr, "adX: assertion failed: %s\n      at %s:%d\n", expression, file, line);
    std::fflush(stderr);
    ADX_DEBUG_BREAK();
    std::abort();
}

AssertHandler setAssertHandler(AssertHandler handler) noexcept {
    return handlerSlot().exchange(handler != nullptr ? handler : &abortOnAssertFailure);
}

void reportAssertFailure(const char* expression, const char* file, int line) noexcept {
    handlerSlot().load(std::memory_order_acquire)(expression, file, line);
}

} // namespace adx::core
