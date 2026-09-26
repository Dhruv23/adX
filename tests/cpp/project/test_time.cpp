// Ticks, the tuplet rounding rule, and strong ids.
#include <catch2/catch_test_macros.hpp>

#include <unordered_set>

#include "engine/core/Ids.h"
#include "engine/core/Rational.h"
#include "engine/core/Time.h"

using adx::core::divideSpan;
using adx::core::kPpq;
using adx::core::Rational;
using adx::core::Ticks;

TEST_CASE("PPQ divides every grid a musician uses", "[time]") {
    // The claim in phase_2.md §4.1, checked rather than asserted. Seven is absent on
    // purpose: no PPQ divides by it, which is why divideSpan exists.
    for (const std::int64_t divisor :
         {2,  3,  4,  5,   6,   8,   10,  12,  15,  16,  20,  24,  30,  32,  40,  48,   60,
          64, 80, 96, 120, 128, 160, 192, 240, 256, 320, 384, 480, 640, 768, 960, 1280, 1920}) {
        INFO("divisor " << divisor);
        CHECK(kPpq % divisor == 0);
    }
    CHECK(kPpq % 7 != 0);
}

TEST_CASE("time_tuplet_determinism", "[time]") {
    // 5-, 7- and 11-tuplets produce identical ticks across a thousand recompiles.
    // Exactness was never the requirement; reproducibility was, because that is what
    // hot-reload and offline-export agreement actually need.
    const Ticks bar{kPpq * 4};

    for (const std::int64_t n : {5, 7, 11}) {
        std::vector<std::int64_t> first;
        for (std::int64_t i = 0; i <= n; ++i) {
            first.push_back(divideSpan(bar, i, n).value);
        }
        for (int pass = 0; pass < 1000; ++pass) {
            for (std::int64_t i = 0; i <= n; ++i) {
                REQUIRE(divideSpan(bar, i, n).value == first[static_cast<std::size_t>(i)]);
            }
        }
        // Endpoints are exact whatever happens in between.
        CHECK(first.front() == 0);
        CHECK(first.back() == bar.value);
        // And the division is monotone, so no two steps of a 7-tuplet collide.
        for (std::size_t i = 1; i < first.size(); ++i) {
            CHECK(first[i] > first[i - 1]);
        }
    }
}

TEST_CASE("divideSpan rounds symmetrically about zero", "[time]") {
    const Ticks span{kPpq * 4};
    for (std::int64_t i = 0; i <= 7; ++i) {
        CHECK(divideSpan(-span, i, 7).value == -divideSpan(span, i, 7).value);
    }
}

TEST_CASE("Rational stays exact", "[time]") {
    CHECK(Rational::make(2, 4) == Rational::make(1, 2));
    CHECK(Rational::make(-2, -4) == Rational::make(1, 2));
    CHECK(Rational::make(1, -2) == Rational::make(-1, 2));
    CHECK(Rational::make(1, 3) + Rational::make(1, 6) == Rational::make(1, 2));
    CHECK(Rational::make(1, 3) < Rational::make(1, 2));
    // A third is not representable in binary floating point, which is the entire
    // reason this type exists.
    CHECK(adx::core::ticksOfWholeNote(Rational::make(1, 16)).value == kPpq / 4);
    CHECK(adx::core::ticksOfWholeNote(Rational::make(1, 12)).value == kPpq / 3);
}

TEST_CASE("ids are distinct types and are never reused", "[ids]") {
    adx::core::IdCounter<adx::core::ChannelTag> counter;
    CHECK_FALSE(adx::core::ChannelId{}.valid());

    const auto first = counter.next();
    const auto second = counter.next();
    CHECK(first.value == 1);
    CHECK(second.value == 2);
    CHECK(first != second);

    // The property the undo gate depends on: restoring a mark makes the next
    // allocation hand out the same id it handed out before.
    const std::uint32_t mark = counter.mark();
    const auto third = counter.next();
    counter.restore(mark);
    CHECK(counter.next() == third);

    // And observing an id from a file keeps the counter above it.
    counter.observe(adx::core::ChannelId{100});
    CHECK(counter.next().value == 101);
}
