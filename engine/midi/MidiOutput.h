// adx-thread: realtime
//
// MIDI output (STATE.md P9-5): the audio thread queues messages stamped with the stream
// frame they belong to, and MidiDevices' sender thread sends each one when that frame
// is heard - the block's wall time (AudioClock) plus the device's output latency - so a
// clock pulse leaves with the beat it marks, not a buffer early.
//
// Messages from the main thread (a controller's LEDs, `adx midi send`) do not come
// through here; they go straight to the port, since nothing they mark is in the audio.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "engine/rt/SpscRing.h"

namespace adx::midi {

/// One outgoing message. Ten bytes holds the longest one the engine writes, MTC's full
/// frame (F0 7F 7F 01 01 hh mm ss ff F7).
struct OutMessage {
    std::uint64_t streamFrame{0};
    std::array<std::uint8_t, 10> bytes{};
    std::uint8_t size{0};
};

/// What a port is sent. Set from the main thread, read by the audio thread each block.
struct OutputSync {
    std::atomic<bool> clock{false};
    std::atomic<bool> mtc{false};
};

class MidiOutput {
public:
    static constexpr std::size_t kRingCapacity = 2048;

    /// AUDIO THREAD. False, and counted as lost, when the ring is full.
    bool push(const OutMessage& message) noexcept {
        if (!m_ring.tryPush(message)) {
            m_lost.fetch_add(1, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }
    /// SENDER THREAD.
    bool pop(OutMessage& out) noexcept {
        return m_ring.tryPop(out);
    }

    [[nodiscard]] OutputSync& sync() noexcept {
        return m_sync;
    }
    [[nodiscard]] const OutputSync& sync() const noexcept {
        return m_sync;
    }
    /// Messages lost to a full ring. Any thread.
    [[nodiscard]] std::uint64_t lostCount() const noexcept {
        return m_lost.load(std::memory_order_acquire);
    }

private:
    rt::SpscRing<OutMessage, kRingCapacity> m_ring;
    OutputSync m_sync;
    std::atomic<std::uint64_t> m_lost{0};
};

} // namespace adx::midi
