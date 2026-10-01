// A fixed-size heap array that realtime code may *hold* but only the main thread may
// size.
//
// Every header under a realtime path is banned from <vector> and <memory>, which is
// right for the callback and a problem for the object that runs in it: a node's voice
// storage, a delay line's history and an effect's lookahead buffer all have to live
// somewhere, sized from the sample rate or the polyphony when the node is prepared.
// Phase 3's nodes need it and every Phase 4 effect will (phase_3.md §9: "any effect
// needing history buffers allocates them in prepare()").
//
// So this is the one owning container a realtime header may name. allocate() and the
// destructor run on the main thread - prepare() and graph teardown - and the
// allocator hook turns a call from inside the callback into a recorded violation, the
// same as any other allocation. What the audio thread gets is view(): a span, which is
// all it ever needed.
#pragma once

#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

#include "engine/rt/RtConfig.h"

namespace adx::rt {

template<class T> class OwnedArray {
    static_assert(std::is_trivially_destructible_v<T>,
                  "the audio thread may drop its view at any time; nothing may need a "
                  "destructor to run on the elements");

public:
    OwnedArray() noexcept = default;

    explicit OwnedArray(std::size_t count) {
        allocate(count);
    }

    ~OwnedArray() {
        release();
    }

    OwnedArray(const OwnedArray&) = delete;
    OwnedArray& operator=(const OwnedArray&) = delete;

    OwnedArray(OwnedArray&& other) noexcept
        : m_data(std::exchange(other.m_data, nullptr)), m_size(std::exchange(other.m_size, 0)) {}

    OwnedArray& operator=(OwnedArray&& other) noexcept {
        if (this != &other) {
            release();
            m_data = std::exchange(other.m_data, nullptr);
            m_size = std::exchange(other.m_size, 0);
        }
        return *this;
    }

    /// Main thread. Replaces the contents with `count` value-initialised elements.
    void allocate(std::size_t count) {
        release();
        if (count == 0) {
            return;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        // The one owning `new` a realtime header contains, and the reason this type
        // exists: the ban list cannot tell prepare() from process(), so the exemption
        // is stated here, once, rather than in every node that needs storage.
        m_data = new T[count]();
        m_size = count;
    }

    [[nodiscard]] std::span<T> view() noexcept {
        return std::span<T>{m_data, m_size};
    }
    [[nodiscard]] std::span<const T> view() const noexcept {
        return std::span<const T>{m_data, m_size};
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return m_size;
    }
    [[nodiscard]] bool empty() const noexcept {
        return m_size == 0;
    }

private:
    void release() noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        delete[] m_data;
        m_data = nullptr;
        m_size = 0;
    }

    T* m_data{nullptr};
    std::size_t m_size{0};
};

} // namespace adx::rt
