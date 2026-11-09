// adx-thread: realtime
#include "engine/midi/sysex/SysexAssembler.h"

namespace adx::midi {

namespace {

constexpr std::uint8_t kStart = 0xF0;
constexpr std::uint8_t kEnd = 0xF7;

[[nodiscard]] constexpr bool isRealtime(std::uint8_t byte) noexcept {
    return byte >= 0xF8;
}

} // namespace

void SysexAssembler::poll(std::int64_t wallNanos) noexcept {
    if (m_active && m_timeout > 0 && wallNanos - m_last > m_timeout) {
        m_active = false;
        m_size = 0;
        m_overflow = false;
        ++m_stats.timedOut;
    }
}

void SysexAssembler::finish() noexcept {
    if (m_overflow) {
        ++m_stats.oversize;
    } else if (m_queue.push(std::span<const std::uint8_t>(m_buffer.data(), m_size))) {
        ++m_stats.completed;
    }
    m_active = false;
    m_size = 0;
    m_overflow = false;
}

std::size_t SysexAssembler::feed(std::int64_t wallNanos,
                                 std::span<const std::uint8_t> bytes) noexcept {
    poll(wallNanos);
    if (bytes.empty()) {
        return 0;
    }
    // A short message that is not part of SysEx: not ours, unless it interrupts one.
    const bool continues = m_active && (bytes[0] < 0x80 || bytes[0] == kEnd);
    if (bytes[0] != kStart && !continues) {
        if (m_active && !isRealtime(bytes[0])) {
            m_active = false;
            m_size = 0;
            m_overflow = false;
            ++m_stats.interrupted;
        }
        return 0;
    }
    m_last = wallNanos;
    std::size_t taken = 0;
    for (const std::uint8_t byte : bytes) {
        ++taken;
        if (byte == kStart) {
            if (m_active) {
                ++m_stats.interrupted;
            }
            m_active = true;
            m_overflow = false;
            m_buffer[0] = byte;
            m_size = 1;
            continue;
        }
        if (!m_active || isRealtime(byte)) {
            continue;
        }
        if (byte != kEnd && byte >= 0x80) {
            // A status byte ends the message without an F7: discard it.
            m_active = false;
            m_size = 0;
            m_overflow = false;
            ++m_stats.interrupted;
            continue;
        }
        if (m_size < m_buffer.size()) {
            m_buffer[m_size++] = byte;
        } else {
            m_overflow = true;
        }
        if (byte == kEnd) {
            finish();
        }
    }
    return taken;
}

} // namespace adx::midi
