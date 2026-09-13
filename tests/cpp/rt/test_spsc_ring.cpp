#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "engine/rt/AllocGuard.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/SpscRing.h"
#include "engine/rt/Violation.h"

using adx::rt::ScopedRtSection;
using adx::rt::SpscRing;
using adx::rt::ViolationKind;
using adx::rt::ViolationLog;

namespace {

struct Event {
    std::uint32_t id{};
    float value{};
};

} // namespace

TEST_CASE("spsc_push_pop_fifo", "[rt]") {
    // Through a real second thread, not a simulated one: the memory ordering is the
    // thing under test, and a single-threaded loop would exercise none of it.
    static constexpr std::uint32_t kItems = 1'000'000;
    auto ring = std::make_unique<SpscRing<Event, 1024>>();

    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        for (std::uint32_t i = 0; i < kItems;) {
            if (ring->tryPush(Event{.id = i, .value = static_cast<float>(i)})) {
                ++i;
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::uint32_t expected = 0;
    bool ordered = true;
    Event received{};
    while (expected < kItems) {
        if (ring->tryPop(received)) {
            if (received.id != expected) {
                ordered = false;
                break;
            }
            ++expected;
        }
    }
    producer.join();

    CHECK(ordered);
    CHECK(expected == kItems);
    CHECK(producerDone.load(std::memory_order_acquire));
    CHECK(ring->emptyApprox());
}

TEST_CASE("spsc_full_returns_false", "[rt]") {
    SpscRing<Event, 8> ring;
    for (std::uint32_t i = 0; i < 8; ++i) {
        CHECK(ring.tryPush(Event{.id = i}));
    }
    // Capacity is fully usable - the counters are monotonic and masked on use, so
    // there is no sacrificial empty slot.
    CHECK(ring.sizeApprox() == 8);
    CHECK_FALSE(ring.tryPush(Event{.id = 99}));
    CHECK(ring.sizeApprox() == 8);

    // And the refusal cost nothing: a growing queue would have allocated here, which
    // is the entire reason this type exists instead of ReaderWriterQueue.
    Event popped{};
    REQUIRE(ring.tryPop(popped));
    CHECK(popped.id == 0);
    CHECK(ring.tryPush(Event{.id = 99}));
}

TEST_CASE("spsc_empty_returns_false", "[rt]") {
    SpscRing<Event, 8> ring;
    Event untouched{.id = 1234, .value = 5.0F};
    CHECK_FALSE(ring.tryPop(untouched));
    CHECK(untouched.id == 1234);
    CHECK(untouched.value == 5.0F);
}

TEST_CASE("spsc_no_alloc_in_rt_section", "[rt]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("allocator hook not compiled in (Release)");
        return;
    }

    auto ring = std::make_unique<SpscRing<Event, 1024>>();
    ViolationLog& log = ViolationLog::instance();
    log.reset();
    {
        const ScopedRtSection section;
        Event popped{};
        for (std::uint32_t i = 0; i < 100'000; ++i) {
            static_cast<void>(ring->tryPush(Event{.id = i}));
            static_cast<void>(ring->tryPop(popped));
        }
    }
    CHECK(log.count(ViolationKind::Allocation) == 0);
    CHECK(log.count() == 0);
}
