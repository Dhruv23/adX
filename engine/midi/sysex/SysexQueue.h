// adx-thread: realtime
//
// SysEx from a MIDI input's device thread to the main thread (phase_11.5.md §4.4). A
// fixed number of fixed-size slots in a single-producer/single-consumer ring: the device
// thread never allocates and never waits, and a full ring drops the message and counts
// it. SysEx never reaches a channel or the audio graph; the main thread drains this at
// frame rate and dispatches by header.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "engine/rt/SpscRing.h"

namespace adx::midi {

/// One whole message, F0 to F7. 8 KiB holds a DX7 32-voice bank (4104 bytes) and the
/// FM-1's OTA frames.
struct SysexMessage {
    static constexpr std::size_t kMaxBytes = 8192;
    std::uint16_t size{0};
    std::array<std::uint8_t, kMaxBytes> bytes{};

    [[nodiscard]] std::span<const std::uint8_t> view() const noexcept {
        return {bytes.data(), size};
    }
};

class SysexQueue {
public:
    static constexpr std::size_t kSlots = 16;

    /// DEVICE THREAD. False, and counted, when the ring is full or the message is empty
    /// or longer than a slot.
    bool push(std::span<const std::uint8_t> message) noexcept {
        if (message.empty() || message.size() > SysexMessage::kMaxBytes) {
            m_dropped.fetch_add(1, std::memory_order_acq_rel);
            return false;
        }
        // Built in place in the producer's scratch slot, then copied into the ring.
        m_scratch.size = static_cast<std::uint16_t>(message.size());
        std::copy(message.begin(), message.end(), m_scratch.bytes.begin());
        if (!m_ring.tryPush(m_scratch)) {
            m_dropped.fetch_add(1, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }

    /// MAIN THREAD. False when empty.
    bool pop(SysexMessage& out) noexcept {
        return m_ring.tryPop(out);
    }

    /// Messages dropped: ring full, or over-size. Any thread.
    [[nodiscard]] std::uint64_t droppedCount() const noexcept {
        return m_dropped.load(std::memory_order_acquire);
    }

private:
    rt::SpscRing<SysexMessage, kSlots> m_ring;
    SysexMessage m_scratch;
    std::atomic<std::uint64_t> m_dropped{0};
};

} // namespace adx::midi
