// A bump allocator over storage reserved once, reset at the top of every callback.
//
// This is what `std::vector<ScheduledEvent> scheduledEvents` and
// `std::vector<ActiveClipRef> activeClips` become - the two lines in
// _archive/src-cpp/src/AudioEngine.cpp (:206 and :372) that allocated on the audio
// thread 86 times a second, and the single worst defect in the archived codebase
// (FINAL_PLAN §3.3.1).
//
// The storage comes from the main thread. Everything here is pointer arithmetic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "engine/core/Config.h"
#include "engine/rt/RtConfig.h"
#include "engine/rt/Violation.h"

namespace adx::rt {

class BlockArena {
public:
    /// An unusable arena. Exists so a class can hold one as a member before its
    /// storage is known; allocate() on it records a violation like any other
    /// overflow.
    BlockArena() noexcept = default;

    /// `storage` must outlive the arena and must not be used for anything else.
    /// Sizing is the caller's problem, which is what highWaterMark() is for.
    BlockArena(std::byte* storage, std::size_t bytes) noexcept
        : m_base(storage), m_capacity(storage != nullptr ? bytes : 0) {}

    /// Hands out `n` default-uninitialised T. Empty span on overflow.
    ///
    /// Overflow records ViolationKind::Unbounded and returns nothing. It never falls
    /// back to malloc: a fallback would make the arena's size untestable, which is
    /// the entire property being bought here.
    template<class T> [[nodiscard]] ADX_RT_HOT std::span<T> allocate(std::size_t n) noexcept {
        static_assert(std::is_trivially_default_constructible_v<T>);
        static_assert(std::is_trivially_destructible_v<T>,
                      "the arena never runs a destructor; reset() just moves a pointer");

        if (n == 0) {
            return {};
        }

        // Aligned on the absolute address, not on the offset. Aligning the offset is
        // only correct when the backing storage is itself suitably aligned, and it is
        // not: a std::array<std::byte, N> on the stack has alignment 1, and
        // std::vector<std::byte> promises only what operator new gives. Getting this
        // wrong produces a span that is correctly aligned on most runs and faults on a
        // SIMD load on the rest - which is exactly how it was found here, as a test
        // that passed alone and failed in the suite.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto baseAddress = reinterpret_cast<std::uintptr_t>(m_base);
        const std::uintptr_t alignedAddress = alignUp(baseAddress + m_offset, alignof(T));
        const std::size_t aligned = static_cast<std::size_t>(alignedAddress - baseAddress);
        const std::size_t bytes = n * sizeof(T);

        // Checked before the add so an absurd `n` cannot wrap into a small number.
        if (aligned > m_capacity || bytes > m_capacity - aligned) {
            ADX_ASSERT(false && "BlockArena overflow - the arena is sized too small");
            ADX_RECORD_VIOLATION(ViolationKind::Unbounded, static_cast<std::uint32_t>(bytes));
            return {};
        }

        // A bump allocator hands back raw bytes to be read as T. Formally this wants
        // C++23's std::start_lifetime_as; in C++20, for trivially constructible T on
        // the platforms adX targets, the cast is what every arena does.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        T* first = reinterpret_cast<T*>(m_base + aligned);
        m_offset = aligned + bytes;
        if (m_offset > m_highWater) {
            m_highWater = m_offset;
        }
        return std::span<T>{first, n};
    }

    /// Called once at the top of each callback. O(1) - nothing is destroyed.
    ADX_RT_HOT void reset() noexcept {
        m_offset = 0;
    }

    /// Where the next allocation would start. Pair with rewind() to scope scratch to
    /// less than a whole callback.
    [[nodiscard]] ADX_RT_HOT std::size_t mark() const noexcept {
        return m_offset;
    }

    /// Releases everything handed out since `mark`. The scheduler brackets each node's
    /// turn with mark/rewind, so the arena has to hold one node's scratch at a time
    /// rather than every node's at once - which is the difference between an arena
    /// sized by the busiest node and one sized by the size of the project.
    ADX_RT_HOT void rewind(std::size_t mark) noexcept {
        if (mark <= m_offset) {
            m_offset = mark;
        }
    }

    /// The most bytes ever handed out between resets.
    ///
    /// Exported so a test can assert the arena is *sized* correctly rather than
    /// merely not overflowing on today's input - the difference between a bound that
    /// holds and a bound that has not been hit yet.
    [[nodiscard]] std::size_t highWaterMark() const noexcept {
        return m_highWater;
    }

    void resetHighWaterMark() noexcept {
        m_highWater = 0;
    }

    [[nodiscard]] std::size_t used() const noexcept {
        return m_offset;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return m_capacity;
    }

private:
    [[nodiscard]] static constexpr std::uintptr_t alignUp(std::uintptr_t value,
                                                          std::size_t alignment) noexcept {
        const auto mask = static_cast<std::uintptr_t>(alignment) - 1;
        return (value + mask) & ~mask;
    }

    std::byte* m_base{nullptr};
    std::size_t m_capacity{0};
    std::size_t m_offset{0};
    std::size_t m_highWater{0};
};

} // namespace adx::rt
