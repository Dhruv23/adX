#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

#include "engine/rt/AllocGuard.h"
#include "engine/rt/FixedString.h"
#include "engine/rt/FixedVector.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"

using adx::rt::FixedString;
using adx::rt::FixedVector;
using adx::rt::ScopedRtSection;
using adx::rt::ViolationKind;
using adx::rt::ViolationLog;

TEST_CASE("fixed_vector_basic_use", "[rt]") {
    FixedVector<int, 4> values;
    CHECK(values.empty());
    CHECK(values.capacity() == 4);

    CHECK(values.pushBack(1));
    CHECK(values.pushBack(2));
    CHECK(values.size() == 2);
    CHECK(values[0] == 1);
    CHECK(values[1] == 2);

    values.popBack();
    CHECK(values.size() == 1);
    values.clear();
    CHECK(values.empty());
}

TEST_CASE("fixed_vector_overflow_asserts", "[rt]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("RT guard not compiled in (Release)");
        return;
    }

    // The assertion handler that records instead of aborting, so a failed ADX_ASSERT
    // inside an RT section is a test failure rather than a dead process.
    adx::rt::installRtGuards();

    FixedVector<int, 2> values;
    ViolationLog& log = ViolationLog::instance();
    log.reset();

    bool fourthAccepted = true;
    {
        const ScopedRtSection section;
        static_cast<void>(values.pushBack(1));
        static_cast<void>(values.pushBack(2));
        fourthAccepted = values.pushBack(3);
    }

    CHECK_FALSE(fourthAccepted);
    CHECK(values.size() == 2);
    // Refused, recorded, and - the point of the whole type - it did not grow.
    CHECK(log.count(ViolationKind::Unbounded) >= 1);
    CHECK(log.count(ViolationKind::Allocation) == 0);
    CHECK(values[0] == 1);
    CHECK(values[1] == 2);
}

TEST_CASE("fixed_string_assigns_and_truncates", "[rt]") {
    FixedString<8> name;
    CHECK(name.empty());
    CHECK(name.capacity() == 7);

    CHECK(name.assign("abc"));
    CHECK(name.view() == "abc");
    CHECK(name.size() == 3);
    CHECK(std::string_view{name.cStr()} == "abc");

    // Truncation is reported, not recorded as a violation: an over-long device name
    // is a display problem, not a realtime one.
    CHECK_FALSE(name.assign("0123456789"));
    CHECK(name.size() == 7);
    CHECK(name.view() == "0123456");
    // Still NUL-terminated, so handing cStr() to a C API stays safe.
    CHECK(std::string_view{name.cStr()}.size() == 7);

    name.clear();
    CHECK(name.empty());
}

TEST_CASE("fixed_string_no_alloc_in_rt_section", "[rt]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("RT guard not compiled in (Release)");
        return;
    }

    FixedString<128> name;
    ViolationLog& log = ViolationLog::instance();
    log.reset();
    {
        const ScopedRtSection section;
        for (int i = 0; i < 1000; ++i) {
            static_cast<void>(name.assign("a moderately long device name, assigned repeatedly"));
        }
    }
    CHECK(log.count() == 0);
}
