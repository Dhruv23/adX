// Reaching the fixture corpus from a test.
//
// `docs/examples/*.adx` are permanent parser regression fixtures (FINAL_PLAN §6):
// four real v1 files, one of them 551 lines exercising nearly the whole extended
// grammar. ADX_REPO_ROOT is defined by tests/cpp/CMakeLists.txt because the test
// binary runs from wherever ctest puts it, not from the source tree.
#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace adx::tests {

[[nodiscard]] inline std::filesystem::path repoRoot() {
    return std::filesystem::path{ADX_REPO_ROOT};
}

/// Reads a file as bytes. Binary mode on purpose: text mode on Windows would eat
/// the carriage returns the byte-identical round-trip test exists to preserve.
[[nodiscard]] inline std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/// Every example, in a fixed order so a failure names the same file on every run.
[[nodiscard]] inline std::vector<std::filesystem::path> corpusPaths() {
    const std::filesystem::path base = repoRoot() / "docs" / "examples";
    return {
        base / "example.adx",
        base / "c418_demo.adx",
        base / "stakillaz_demo.adx",
        base / "suffocation.adx",
    };
}

} // namespace adx::tests
