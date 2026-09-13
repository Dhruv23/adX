// The most important test in this phase.
//
// Every other realtime test in this project, in every later phase, is meaningless if
// the allocator hook is silently inert - a broken hook makes ten thousand tests pass.
// So this file proves the detector detects, before anything else asks it to.
//
// The [000-guard] tag sorts it first when the suite runs in declaration order, and
// the positive control aborts the run rather than merely failing, because the
// remaining results would be worthless.

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "engine/rt/AllocGuard.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"

using adx::rt::allocGuardCompiledIn;
using adx::rt::allocGuardLinked;
using adx::rt::ScopedRtSection;
using adx::rt::ViolationKind;
using adx::rt::ViolationLog;

namespace {

/// Allocation counts observed around a piece of code, gathered *outside* the RT
/// section and asserted on afterwards.
///
/// Catch2's REQUIRE allocates - it builds an expression string - so asserting inside
/// a ScopedRtSection would record violations of the test's own making and prove
/// nothing about the code under test.
struct AllocationDelta {
    std::uint64_t allocations{};
    std::uint64_t deallocations{};
};

template<class Body> AllocationDelta measure(Body&& body) {
    ViolationLog& log = ViolationLog::instance();
    log.reset();
    {
        const ScopedRtSection section;
        body();
    }
    return AllocationDelta{.allocations = log.count(ViolationKind::Allocation),
                           .deallocations = log.count(ViolationKind::Deallocation)};
}

/// Defeats the optimizer. Without it, `new int` with an immediately discarded result
/// is removable under C++14's allocation-elision rules, and the positive control
/// would fail for a reason that has nothing to do with the hook.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
volatile void* g_sink = nullptr;

} // namespace

TEST_CASE("alloc_guard_positive_control", "[rt][000-guard]") {
    if (!allocGuardCompiledIn()) {
        // Release. The guard is compiled out on purpose, so a clean run here would
        // mean nothing at all. Say so loudly rather than passing quietly - a green
        // Release suite must never be mistaken for evidence of realtime safety.
        std::puts("\n[adX] SKIPPED: the allocator hook is compiled out in this configuration.\n"
                  "      A passing realtime suite here proves nothing. Run Debug or\n"
                  "      RelWithDebInfo for the realtime gate.\n");
        SUCCEED("allocator hook not compiled in (Release)");
        return;
    }

    // Compiled in is not the same as linked in: the linker only pulls an object file
    // out of a static library when something references a symbol in it, and the CRT
    // already supplies operator new.
    REQUIRE(allocGuardLinked());

    const AllocationDelta delta = measure([] {
        // The deliberate violation. If this is not recorded, the hook is inert and
        // every realtime assertion in the project is decoration.
        int* leaked = new int(7);
        g_sink = leaked;
        delete leaked;
    });

    if (delta.allocations == 0) {
        // Not a plain REQUIRE: if the hook is inert, every later result in this
        // binary is worthless, so stop the run rather than produce a long report of
        // tests that were never actually checking anything.
        FAIL("the allocator hook did not record a deliberate allocation inside an RT "
             "section. Every realtime test in this suite is now meaningless. Check that "
             "AllocGuard.cpp is linked and ADX_ENABLE_RT_GUARD is on.");
    }

    CHECK(delta.allocations >= 1);
    CHECK(delta.deallocations >= 1);
}

TEST_CASE("alloc_guard_negative_control", "[rt]") {
    if (!allocGuardCompiledIn()) {
        SUCCEED("allocator hook not compiled in (Release)");
        return;
    }

    ViolationLog& log = ViolationLog::instance();
    log.reset();

    // The same allocation, outside any RT section. Recording this would mean the hook
    // fires on everything, which would make a non-zero count meaningless in the other
    // direction.
    int* allocated = new int(7);
    g_sink = allocated;
    delete allocated;

    CHECK(log.count(ViolationKind::Allocation) == 0);
    CHECK(log.count(ViolationKind::Deallocation) == 0);
}

TEST_CASE("alloc_guard_catches_all_forms", "[rt]") {
    if (!allocGuardCompiledIn()) {
        SUCCEED("allocator hook not compiled in (Release)");
        return;
    }

    // Over-aligned, so the aligned operator new forms are the ones selected. A missing
    // form is a hole in the coverage that looks exactly like safety.
#if defined(_MSC_VER)
#    pragma warning(push)
// C4324: padded due to alignment specifier. Being padded is the entire reason this
// type exists - it is what makes the compiler reach for the aligned new.
#    pragma warning(disable : 4324)
#endif
    struct alignas(64) OverAligned {
        double value;
    };
#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

    SECTION("plain new and new[]") {
        const AllocationDelta delta = measure([] {
            int* one = new int(1);
            g_sink = one;
            delete one;
            int* many = new int[8];
            g_sink = many;
            delete[] many;
        });
        CHECK(delta.allocations >= 2);
        CHECK(delta.deallocations >= 2);
    }

    SECTION("nothrow new and new[]") {
        const AllocationDelta delta = measure([] {
            int* one = new (std::nothrow) int(1);
            g_sink = one;
            ::operator delete(one, std::nothrow);
            int* many = new (std::nothrow) int[8];
            g_sink = many;
            ::operator delete[](many, std::nothrow);
        });
        CHECK(delta.allocations >= 2);
        CHECK(delta.deallocations >= 2);
    }

    SECTION("aligned new and new[]") {
        const AllocationDelta delta = measure([] {
            auto* one = new OverAligned{.value = 1.0};
            g_sink = one;
            delete one;
            auto* many = new OverAligned[4];
            g_sink = many;
            delete[] many;
        });
        CHECK(delta.allocations >= 2);
        CHECK(delta.deallocations >= 2);
    }

    SECTION("aligned nothrow new") {
        const AllocationDelta delta = measure([] {
            auto* one = new (std::nothrow) OverAligned;
            g_sink = one;
            ::operator delete(one, std::align_val_t{alignof(OverAligned)}, std::nothrow);
        });
        CHECK(delta.allocations >= 1);
        CHECK(delta.deallocations >= 1);
    }
}

TEST_CASE("alloc_guard_catches_std_containers", "[rt]") {
    if (!allocGuardCompiledIn()) {
        SUCCEED("allocator hook not compiled in (Release)");
        return;
    }

    // This is iteration one's defect, reproduced as a test.
    //
    // _archive/src-cpp/src/AudioEngine.cpp:206 and :372 constructed
    // std::vector<ScheduledEvent> and std::vector<ActiveClipRef> on every audio
    // callback - 86 mallocs a second on the realtime thread, and the single worst
    // defect in the archived codebase (FINAL_PLAN §3.3.1). It survived because
    // nothing could see it. Now something can.
    const AllocationDelta vectorDelta = measure([] {
        std::vector<int> scheduled;
        scheduled.push_back(1);
        g_sink = scheduled.data();
    });
    CHECK(vectorDelta.allocations >= 1);

    // std::string is the same defect wearing a different hat, and it is the reason
    // the realtime ban list bans <string> as well as <vector>.
    const AllocationDelta stringDelta = measure([] {
        std::string name = "a string long enough to defeat the small-string optimisation";
        g_sink = name.data();
    });
    CHECK(stringDelta.allocations >= 1);

    const AllocationDelta sharedDelta = measure([] {
        auto shared = std::make_shared<int>(3);
        g_sink = shared.get();
    });
    CHECK(sharedDelta.allocations >= 1);
}

TEST_CASE("alloc_guard_records_useful_detail", "[rt]") {
    if (!allocGuardCompiledIn()) {
        SUCCEED("allocator hook not compiled in (Release)");
        return;
    }

    ViolationLog& log = ViolationLog::instance();
    log.reset();
    {
        const ScopedRtSection section;
        int* leaked = new int(7);
        g_sink = leaked;
        delete leaked;
    }

    const auto records = log.snapshot();
    REQUIRE_FALSE(records.empty());

    const auto& first = records.front();
    CHECK(first.kind == ViolationKind::Allocation);
    CHECK(first.sizeBytes == sizeof(int));
    CHECK(first.returnAddrCount == 1);
    // The captured address is the *caller's*, which is what makes the record say
    // which code allocated rather than merely that something did.
    CHECK(first.returnAddrs[0] != nullptr);
}
