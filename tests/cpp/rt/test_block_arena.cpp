#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/rt/AllocGuard.h"
#include "engine/rt/BlockArena.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"

using adx::rt::BlockArena;
using adx::rt::ScopedRtSection;
using adx::rt::ViolationKind;
using adx::rt::ViolationLog;

namespace {

struct alignas(32) Wide {
    double a;
    double b;
    double c;
    double d;
};

} // namespace

TEST_CASE("block_arena_bump_and_reset", "[rt]") {
    // Deliberately unaligned storage, offset by one byte inside an aligned buffer.
    // std::array<std::byte, N> has alignment 1 and std::vector<std::byte> promises
    // only what operator new gives, so an arena that aligns offsets rather than
    // addresses is correct only by luck - and this is the test that says so.
    alignas(64) std::array<std::byte, 4096> backing{};
    BlockArena arena{backing.data() + 1, backing.size() - 1};

    const auto ints = arena.allocate<std::int32_t>(10);
    REQUIRE(ints.size() == 10);
    CHECK(reinterpret_cast<std::uintptr_t>(ints.data()) % alignof(std::int32_t) == 0);

    // Alignment is honoured, which is the part a naive bump allocator gets wrong and
    // only discovers on a machine that faults on unaligned SIMD loads.
    const auto wides = arena.allocate<Wide>(2);
    REQUIRE(wides.size() == 2);
    CHECK(reinterpret_cast<std::uintptr_t>(wides.data()) % alignof(Wide) == 0);

    const std::size_t peak = arena.used();
    CHECK(arena.highWaterMark() == peak);

    arena.reset();
    CHECK(arena.used() == 0);
    // The high-water mark survives the reset - that is what makes it a statement
    // about how the arena is *sized* rather than about this one callback.
    CHECK(arena.highWaterMark() == peak);

    const auto again = arena.allocate<std::int32_t>(4);
    CHECK(again.size() == 4);
    CHECK(arena.highWaterMark() == peak);
}

TEST_CASE("block_arena_overflow_is_violation", "[rt]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("RT guard not compiled in (Release)");
        return;
    }
    adx::rt::installRtGuards();

    std::array<std::byte, 64> storage{};
    BlockArena arena{storage.data(), storage.size()};

    ViolationLog& log = ViolationLog::instance();
    log.reset();

    std::size_t granted = 1;
    {
        const ScopedRtSection section;
        granted = arena.allocate<std::int32_t>(1000).size();
    }

    CHECK(granted == 0);
    CHECK(log.count(ViolationKind::Unbounded) >= 1);
    // Never a malloc fallback. A fallback would make the arena's size untestable,
    // which is the entire property being bought here.
    CHECK(log.count(ViolationKind::Allocation) == 0);
}

TEST_CASE("block_arena_zero_and_empty", "[rt]") {
    std::array<std::byte, 64> storage{};
    BlockArena arena{storage.data(), storage.size()};
    CHECK(arena.allocate<int>(0).empty());
    CHECK(arena.used() == 0);

    // A default-constructed arena has no storage; asking it for anything must fail
    // like any other overflow rather than hand back a null span nobody checks.
    const BlockArena unusable;
    CHECK(unusable.capacity() == 0);
}
