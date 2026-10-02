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

/// True on a shared CI runner running an unoptimised build, where a callback can be
/// descheduled for tens of milliseconds (Debug run 36833200360: 36 of 11251 over, worst
/// 44 ms). Wall-clock deadline gates allow 1 % over there and stay strict everywhere
/// else, so a local run or an optimised CI job still demands zero.
[[nodiscard]] inline bool deadlineSlackAllowed() {
#ifdef NDEBUG
    return false;
#else
    return environment("CI").has_value();
#endif
}

} // namespace adx::tests
