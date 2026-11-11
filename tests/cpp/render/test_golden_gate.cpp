// The Phase 12 regression gate (phase_12.md §5, golden_unchanged_by_phase12).
//
// Phase 12 adds parameters to existing instruments and effects, and every one of them
// has to default to inert. tests/golden/baseline_phase11/ is the four hash files as
// they were before the phase's first commit. Every hash in them must still be the
// hash committed now, and the golden tests beside this one check that the committed
// hashes are what each configuration renders.

#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include "tests/cpp/render/GoldenHashes.h"

TEST_CASE("golden_unchanged_by_phase12", "[render][golden]") {
    std::size_t checked = 0;
    for (const std::string_view file :
         {"hashes.txt", "instruments.txt", "effects.txt", "formats.txt"}) {
        const auto baseline =
            adx::tests::readHashes(adx::tests::goldenDirectory() / "baseline_phase11" / file);
        const auto current = adx::tests::readHashes(adx::tests::goldenDirectory() / file);
        REQUIRE_FALSE(baseline.empty());
        for (const auto& [name, hash] : baseline) {
            INFO(file << ": " << name);
            const auto found = current.find(name);
            REQUIRE(found != current.end());
            CHECK(found->second == hash);
            ++checked;
        }
    }
    CHECK(checked == 179);
}
