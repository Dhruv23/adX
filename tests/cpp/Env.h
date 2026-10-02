// Reading an environment variable from a test, without MSVC's getenv deprecation.
//
// Test tooling only: the opt-ins here (a fuzz budget, a golden-corpus rewrite) have to
// be something a developer or a scheduled job can set for one run. Engine code never
// reads the environment.
#pragma once

#include <cstdlib>
#include <optional>
#include <string>

namespace adx::tests {

[[nodiscard]] inline std::optional<std::string> environment(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string out{value};
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(value);
    return out;
#else
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>{value};
#endif
}

/// True on a shared CI runner, where the OS can deschedule a callback for tens of
/// milliseconds whatever the build: Debug run 36833200360 had 36 of 11251 over (worst
/// 44 ms), RelWithDebInfo run 37066275031 6 of 11253 (10 ms), Release run 37074455498
/// 4 of 11254 (23 ms) - on a project of test tones that takes a fraction of a
/// millisecond per callback. Wall-clock deadline gates allow 1 % over there. Local runs
/// stay strict, and the phase-completion run is local (plans/STATE.md, step 5).
[[nodiscard]] inline bool deadlineSlackAllowed() {
    return environment("CI").has_value();
}

} // namespace adx::tests
