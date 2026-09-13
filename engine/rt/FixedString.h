// A name the audio thread can see.
//
// std::string is banned below the callback because it allocates, and because a
// snapshot published to the audio thread has to be trivially copyable to be safe to
// hand over at all. Every RT-visible name - a device name, a channel name, an
// insert label - is one of these.
//
// Phase 2 inherits this: the main-thread project model may use std::string freely,
// but anything that has to reach the render snapshot flattens into a FixedString
// (phase_1.md §8).
#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "engine/rt/RtConfig.h"

namespace adx::rt {

template<std::size_t Capacity> class FixedString {
    static_assert(Capacity > 1, "one byte is reserved for the terminator");

public:
    FixedString() noexcept = default;

    explicit FixedString(std::string_view text) noexcept {
        assign(text);
    }

    /// Copies as much of `text` as fits and terminates. Returns false if it had to
    /// truncate.
    ///
    /// Truncation is reported rather than recorded as a violation: an
    /// over-long device name is a display problem, not a realtime one, and callers
    /// that care can check. Nothing here can fail in a way the audio thread needs to
    /// know about.
    bool assign(std::string_view text) noexcept {
        const std::size_t maxChars = Capacity - 1;
        const bool truncated = text.size() > maxChars;
        m_size = truncated ? maxChars : text.size();
        if (m_size > 0) {
            std::memcpy(m_chars.data(), text.data(), m_size);
        }
        m_chars[m_size] = '\0';
        return !truncated;
    }

    void clear() noexcept {
        m_size = 0;
        m_chars[0] = '\0';
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view{m_chars.data(), m_size};
    }

    /// Always NUL-terminated, so this is safe to hand to a C API.
    [[nodiscard]] const char* cStr() const noexcept {
        return m_chars.data();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return m_size;
    }
    [[nodiscard]] bool empty() const noexcept {
        return m_size == 0;
    }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity - 1;
    }

    [[nodiscard]] friend bool operator==(const FixedString& lhs, const FixedString& rhs) noexcept {
        return lhs.view() == rhs.view();
    }

private:
    std::array<char, Capacity> m_chars{};
    std::size_t m_size{0};
};

} // namespace adx::rt
