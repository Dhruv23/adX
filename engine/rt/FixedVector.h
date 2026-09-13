// A vector that cannot grow.
//
// This is the type that replaces every `std::vector` the audio thread touches. The
// point is not that it is faster; it is that its capacity is a number somebody chose
// and a test can assert on, rather than something malloc decides at 86 callbacks a
// second (FINAL_PLAN §3.3.1).
//
// Overflow is a recorded Unbounded violation and a refused push - never a
// reallocation, never a silent drop.
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <type_traits>

#include "engine/core/Config.h"
#include "engine/rt/RtConfig.h"
#include "engine/rt/Violation.h"

namespace adx::rt {

template<class T, std::size_t Capacity> class FixedVector {
    // Trivially copyable and default constructible, so the whole backing array can
    // be created up front and no element ever needs a constructor or a destructor
    // run on the audio thread. Anything that needs richer lifetime than that does
    // not belong below the callback.
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_default_constructible_v<T>);
    static_assert(Capacity > 0);

public:
    using value_type = T;

    [[nodiscard]] ADX_RT_HOT bool pushBack(const T& value) noexcept {
        if (m_size >= Capacity) {
            ADX_ASSERT(m_size < Capacity);
            ADX_RECORD_VIOLATION(ViolationKind::Unbounded, static_cast<std::uint32_t>(sizeof(T)));
            return false;
        }
        m_storage[m_size] = value;
        ++m_size;
        return true;
    }

    ADX_RT_HOT void popBack() noexcept {
        ADX_ASSERT(m_size > 0);
        if (m_size > 0) {
            --m_size;
        }
    }

    /// Drops every element. Trivial types, so this is a counter reset - the backing
    /// storage keeps whatever it held, which is why data() is only valid up to
    /// size().
    ADX_RT_HOT void clear() noexcept {
        m_size = 0;
    }

    [[nodiscard]] ADX_RT_HOT T& operator[](std::size_t index) noexcept {
        ADX_ASSERT(index < m_size);
        return m_storage[index];
    }

    [[nodiscard]] ADX_RT_HOT const T& operator[](std::size_t index) const noexcept {
        ADX_ASSERT(index < m_size);
        return m_storage[index];
    }

    [[nodiscard]] std::span<T> view() noexcept {
        return std::span<T>{m_storage.data(), m_size};
    }

    [[nodiscard]] std::span<const T> view() const noexcept {
        return std::span<const T>{m_storage.data(), m_size};
    }

    [[nodiscard]] T* begin() noexcept {
        return m_storage.data();
    }
    [[nodiscard]] T* end() noexcept {
        return m_storage.data() + m_size;
    }
    [[nodiscard]] const T* begin() const noexcept {
        return m_storage.data();
    }
    [[nodiscard]] const T* end() const noexcept {
        return m_storage.data() + m_size;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_size;
    }
    [[nodiscard]] bool empty() const noexcept {
        return m_size == 0;
    }
    [[nodiscard]] bool full() const noexcept {
        return m_size == Capacity;
    }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    std::array<T, Capacity> m_storage{};
    std::size_t m_size{0};
};

} // namespace adx::rt
