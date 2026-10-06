#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "engine/rt/OverwriteRing.h"

using adx::rt::OverwriteRing;

namespace {

/// A frame carrying a known ramp value, so a reader can tell whether what it read is
/// a contiguous run of what the writer wrote.
struct RampFrame {
    std::uint64_t index{};
};

} // namespace

TEST_CASE("overwrite_ring_latest_ordering", "[rt]") {
    OverwriteRing<RampFrame, 16> ring;
    for (std::uint64_t i = 0; i < 16; ++i) {
        ring.write(RampFrame{.index = i});
    }

    std::array<RampFrame, 4> out{};
    const std::size_t got = ring.readLatest(out.data(), out.size());
    REQUIRE(got == 4);
    // Oldest first, and "latest" means the last four written.
    CHECK(out[0].index == 12);
    CHECK(out[1].index == 13);
    CHECK(out[2].index == 14);
    CHECK(out[3].index == 15);
}

TEST_CASE("overwrite_ring_partial_fill", "[rt]") {
    OverwriteRing<RampFrame, 16> ring;
    ring.write(RampFrame{.index = 0});
    ring.write(RampFrame{.index = 1});

    std::array<RampFrame, 8> out{};
    const std::size_t got = ring.readLatest(out.data(), out.size());
    // min(count, written) before the first wrap - a scope opened one frame after the
    // stream starts must not read whatever the buffer was initialised with.
    CHECK(got == 2);
    CHECK(out[0].index == 0);
    CHECK(out[1].index == 1);
}

TEST_CASE("overwrite_ring_wraps", "[rt]") {
    OverwriteRing<RampFrame, 16> ring;
    for (std::uint64_t i = 0; i < 48; ++i) { // three times capacity
        ring.write(RampFrame{.index = i});
    }
    CHECK(ring.framesWritten() == 48);

    std::array<RampFrame, 16> out{};
    const std::size_t got = ring.readLatest(out.data(), out.size());
    REQUIRE(got == 16);
    for (std::size_t i = 0; i < got; ++i) {
        // Only the newest window survives; the writer never blocked to preserve the
        // rest, which is the contract.
        CHECK(out[i].index == 32 + i);
    }
}

TEST_CASE("overwrite_ring_readLatest_caps_at_capacity", "[rt]") {
    OverwriteRing<RampFrame, 16> ring;
    for (std::uint64_t i = 0; i < 100; ++i) {
        ring.write(RampFrame{.index = i});
    }
    std::array<RampFrame, 64> out{};
    // Asking for more than the ring holds returns what it holds, not garbage past it.
    CHECK(ring.readLatest(out.data(), out.size()) == 16);
}

TEST_CASE("overwrite_ring_concurrent", "[rt][.slow]") {
    // A writer at audio rate and a reader at UI rate, for a few seconds (phase_1.md:
    // "1 writer at 48 kHz + 1 reader at 60 Hz"). The assertion is not "the reader saw
    // everything" - it is allowed to miss data - but that everything it did see was a
    // contiguous, non-decreasing run of the ramp. A torn frame or a mis-ordered window
    // would break that.
    //
    // The ring's contract allows one exception: a reader overtaken mid-copy may see a
    // seam. A read is counted as overtaken when the writer advanced by more than the
    // ring's spare capacity while it copied; those reads are excluded from the
    // contiguity check, and must be rare. The writer used to spin unthrottled - millions
    // of frames a second, not 48 kHz - which made overtakes routine on a loaded CI
    // runner and failed this test there (CI run 37502991839, Release).
    auto ring = std::make_unique<OverwriteRing<RampFrame, 8192>>();
    std::atomic<bool> running{true};

    std::thread writer([&] {
        using Clock = std::chrono::steady_clock;
        constexpr std::uint64_t kBlock = 256; // 5.33 ms at 48 kHz
        const auto period = std::chrono::nanoseconds(kBlock * 1'000'000'000ULL / 48'000ULL);
        auto next = Clock::now();
        std::uint64_t i = 0;
        while (running.load(std::memory_order_acquire)) {
            for (std::uint64_t k = 0; k < kBlock; ++k) {
                ring->write(RampFrame{.index = i});
                ++i;
            }
            next += period;
            std::this_thread::sleep_until(next);
        }
    });

    bool contiguous = true;
    int overtaken = 0;
    int reads = 0;
    std::uint64_t lastSeen = 0;
    std::array<RampFrame, 512> window{};
    for (int read = 0; read < 180 && contiguous; ++read) { // ~3 s at 60 Hz
        const std::uint64_t before = ring->framesWritten();
        const std::size_t got = ring->readLatest(window.data(), window.size());
        const std::uint64_t after = ring->framesWritten();
        ++reads;
        if (after - before > OverwriteRing<RampFrame, 8192>::capacity() - got) {
            ++overtaken; // the documented seam case: nothing to check in this window
        } else {
            for (std::size_t i = 1; i < got; ++i) {
                if (window[i].index != window[i - 1].index + 1) {
                    contiguous = false;
                    break;
                }
            }
        }
        if (got > 0) {
            CHECK(window[got - 1].index >= lastSeen);
            lastSeen = window[got - 1].index;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    running.store(false, std::memory_order_release);
    writer.join();

    INFO(overtaken << " of " << reads << " reads were overtaken by the writer");
    CHECK(contiguous);
    CHECK(overtaken * 10 <= reads);
    CHECK(lastSeen > 0);
}
