// Bounded single-producer/single-consumer queue. Wait-free on both sides, and it
// never allocates - not at construction, not when full, not ever.
//
// This is the UI-thread-to-audio-thread channel. Iteration one used
// moodycamel::ReaderWriterQueue, which grows by allocating a new block when full.
// Growth on the producer side is safe in itself, but it makes "the queue is full"
// an invisible malloc instead of a designed, testable state - and the project needs
// the hard bound far more than it needs the elasticity.
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>

#include "engine/rt/RtConfig.h"

namespace adx::rt {

#if defined(_MSC_VER)
#    pragma warning(push)
// C4324: "structure was padded due to alignment specifier". The padding is the
// entire purpose of the alignas below - it is what keeps the producer's counter,
// the consumer's counter and the data off each other's cache lines. Warning about
// it is like warning that a lock is slow: true, intended, and not actionable.
#    pragma warning(disable : 4324)
#endif

template<class T, std::size_t Capacity> class SpscRing {
    // Kept verbatim from _archive/src-cpp/include/AudioData.h, which was right to
    // insist on it: anything crossing to the audio thread has to be copyable with a
    // memcpy, because the consumer must not run a constructor or a destructor.
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscRing carries data across a thread boundary by value");
    static_assert(std::is_default_constructible_v<T>);
    static_assert(std::has_single_bit(Capacity), "Capacity must be a power of two");

public:
    using value_type = T;

    /// Producer side. False when full; the caller decides what that means.
    [[nodiscard]] ADX_RT_HOT bool tryPush(const T& value) noexcept {
        // Relaxed on our own counter - no other thread writes it. Acquire on the
        // consumer's, so the slot we are about to overwrite is known to have been
        // read.
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        const std::size_t head = m_head.load(std::memory_order_acquire);
        if (tail - head >= Capacity) {
            return false;
        }
        m_slots[tail & kMask] = value;
        // Release: the slot write above must be visible before the consumer can see
        // the new tail.
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    /// Consumer side. False when empty, and `out` is left untouched.
    [[nodiscard]] ADX_RT_HOT bool tryPop(T& out) noexcept {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        if (head == tail) {
            return false;
        }
        out = m_slots[head & kMask];
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    /// How many items are queued, as of some point during the call. Both endpoints
    /// move independently, so treat it as a hint - useful for a fill-level meter,
    /// not for deciding whether the next push will succeed.
    [[nodiscard]] std::size_t sizeApprox() const noexcept {
        const std::size_t tail = m_tail.load(std::memory_order_acquire);
        const std::size_t head = m_head.load(std::memory_order_acquire);
        return tail - head;
    }

    [[nodiscard]] bool emptyApprox() const noexcept {
        return sizeApprox() == 0;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // Counters monotonically increase and are masked on use, so the ring holds a
    // full Capacity items rather than Capacity-1, and wrap-around of the counters
    // themselves is a non-event: the subtraction is correct modulo 2^64.
    //
    // Separate cache lines for the two counters, and a third for the data. The UI
    // thread writing m_tail must not invalidate the line the audio thread is
    // reading m_head from.
    alignas(kCacheLine) std::atomic<std::size_t> m_head{0};
    alignas(kCacheLine) std::atomic<std::size_t> m_tail{0};
    alignas(kCacheLine) std::array<T, Capacity> m_slots{};
};

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

} // namespace adx::rt
