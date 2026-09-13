// Phase 0's entire C++ test surface. Its job is to prove the harness runs and
// that the single source of truth for the version reaches C++ intact - there is no
// engine code yet to test (phase_0.md §5).
//
// Also the only translation unit that includes engine/core/Config.h, which is how
// ADX_ASSERT gets compiled and linted before Phase 1 depends on it.

#include <catch2/catch_test_macros.hpp>

#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

#include "engine/core/Config.h"
#include "engine/core/Version.h"

namespace {

struct SemanticVersion {
    int major{};
    int minor{};
    int patch{};
};

/// Parses exactly "MAJOR.MINOR.PATCH" and nothing looser. std::from_chars rather
/// than std::stof or std::stoi: no throwing, no locale, and it reports where it
/// stopped, which is the discipline the v2 parser needs in Phase 2 (FINAL_PLAN
/// §3.3.11).
std::optional<int> parseComponent(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    int value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const std::from_chars_result result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last) {
        return std::nullopt;
    }
    return value;
}

std::optional<SemanticVersion> parseSemanticVersion(std::string_view text) {
    ADX_ASSERT(text.data() != nullptr);

    const std::size_t firstDot = text.find('.');
    if (firstDot == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t secondDot = text.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos) {
        return std::nullopt;
    }
    if (text.find('.', secondDot + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    const std::optional<int> major = parseComponent(text.substr(0, firstDot));
    const std::optional<int> minor =
        parseComponent(text.substr(firstDot + 1, secondDot - firstDot - 1));
    const std::optional<int> patch = parseComponent(text.substr(secondDot + 1));
    if (!major || !minor || !patch) {
        return std::nullopt;
    }
    return SemanticVersion{.major = *major, .minor = *minor, .patch = *patch};
}

} // namespace

TEST_CASE("version_is_populated") {
    const std::string_view reported = adx::version();
    REQUIRE_FALSE(reported.empty());

    // Checked with an if rather than REQUIRE, and unwrapped once at the top rather
    // than inside each SECTION. REQUIRE aborts by throwing, which clang-tidy's
    // dataflow analysis cannot see, so every later read of the optional - .value()
    // included - reads to it as unchecked access.
    const std::optional<SemanticVersion> parsed = parseSemanticVersion(reported);
    if (!parsed.has_value()) {
        FAIL("adx::version() is not MAJOR.MINOR.PATCH: " << reported);
        return;
    }
    const SemanticVersion version = *parsed;

    SECTION("it agrees with the ADX_VERSION_* macros") {
        CHECK(version.major == ADX_VERSION_MAJOR);
        CHECK(version.minor == ADX_VERSION_MINOR);
        CHECK(version.patch == ADX_VERSION_PATCH);
        CHECK(reported == std::string_view{ADX_VERSION_STRING});
    }

    SECTION("the commit is recorded, even if only as \"unknown\"") {
        CHECK_FALSE(adx::gitSha().empty());
    }
}
