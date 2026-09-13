#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>

#include "engine/rt/AllocGuard.h"
#include "engine/rt/Reaper.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"

using adx::rt::Reaper;
using adx::rt::retireOf;
using adx::rt::ScopedRtSection;
using adx::rt::ViolationKind;
using adx::rt::ViolationLog;

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
std::atomic<int> g_liveCount{0};

/// Counts its own construction and destruction, so a test can say *when* it died
/// rather than merely that it did.
struct Tracked {
    Tracked() {
        g_liveCount.fetch_add(1, std::memory_order_relaxed);
    }
    ~Tracked() {
        g_liveCount.fetch_sub(1, std::memory_order_relaxed);
    }
    Tracked(const Tracked&) = delete;
    Tracked& operator=(const Tracked&) = delete;
    Tracked(Tracked&&) = delete;
    Tracked& operator=(Tracked&&) = delete;
};

} // namespace

TEST_CASE("reaper_retire_does_not_free", "[rt]") {
    g_liveCount.store(0, std::memory_order_relaxed);
    Reaper reaper;

    auto* tracked = new Tracked();
    CHECK(g_liveCount.load(std::memory_order_relaxed) == 1);

    CHECK(reaper.retire(retireOf(tracked)));
    // Still alive. The audio thread handed over ownership and did not call delete -
    // which is the entire point (FINAL_PLAN §3.2).
    CHECK(g_liveCount.load(std::memory_order_relaxed) == 1);
    CHECK(reaper.pendingApprox() == 1);

    CHECK(reaper.drain() == 1);
    CHECK(g_liveCount.load(std::memory_order_relaxed) == 0);
    CHECK(reaper.pendingApprox() == 0);
}

TEST_CASE("reaper_drains_on_main_thread", "[rt]") {
    g_liveCount.store(0, std::memory_order_relaxed);
    Reaper reaper;

    for (int i = 0; i < 100; ++i) {
        CHECK(reaper.retire(retireOf(new Tracked())));
    }
    CHECK(g_liveCount.load(std::memory_order_relaxed) == 100);

    CHECK(reaper.drain() == 100);
    CHECK(g_liveCount.load(std::memory_order_relaxed) == 0);

    // Exactly once: a second drain finds nothing rather than double-destroying.
    CHECK(reaper.drain() == 0);
    CHECK(g_liveCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("reaper_retire_does_not_allocate", "[rt]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("RT guard not compiled in (Release)");
        return;
    }

    g_liveCount.store(0, std::memory_order_relaxed);
    Reaper reaper;

    // Allocated outside the section - the audio thread is retiring objects somebody
    // else created, which is the real usage.
    std::array<Tracked*, 64> objects{};
    for (auto& object : objects) {
        object = new Tracked();
    }

    ViolationLog& log = ViolationLog::instance();
    log.reset();
    {
        const ScopedRtSection section;
        for (auto* object : objects) {
            static_cast<void>(reaper.retire(retireOf(object)));
        }
    }
    CHECK(log.count() == 0);

    CHECK(reaper.drain() == objects.size());
    CHECK(g_liveCount.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("reaper_full_queue_records_violation", "[rt]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("RT guard not compiled in (Release)");
        return;
    }
    adx::rt::installRtGuards();

    g_liveCount.store(0, std::memory_order_relaxed);
    Reaper reaper;
    ViolationLog& log = ViolationLog::instance();
    log.reset();

    // One past capacity, with nobody draining. A full ring means the main thread has
    // stopped collecting, which is a real fault - so it is recorded rather than
    // leaking in silence.
    std::size_t accepted = 0;
    std::size_t refused = 0;
    Tracked* refusedObject = nullptr;
    {
        const ScopedRtSection section;
        for (std::size_t i = 0; i < Reaper::kCapacity + 1; ++i) {
            auto* object = new Tracked();
            if (reaper.retire(retireOf(object))) {
                ++accepted;
            } else {
                ++refused;
                refusedObject = object;
            }
        }
    }

    CHECK(accepted == Reaper::kCapacity);
    CHECK(refused == 1);
    CHECK(log.count(ViolationKind::Unbounded) >= 1);

    CHECK(reaper.drain() == Reaper::kCapacity);
    delete refusedObject;
    CHECK(g_liveCount.load(std::memory_order_relaxed) == 0);
}
