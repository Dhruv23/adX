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

} // namespace adx::tests
