// adx-thread: realtime
//
// Joins SysEx that arrives in pieces (phase_11.5.md §4.4). RtMidi on Windows delivers a
// whole message, but Bluetooth MIDI and some drivers split one across callbacks, and a
// continuation piece starts with a data byte, not F0. The assembler owns one fixed
// buffer; a message completes on F7 and is handed to the queue.
//
// What it refuses, each counted:
// - oversize: more than SysexMessage::kMaxBytes before F7 (dropped at the F7);
// - interrupted: a new status byte (other than a realtime one, F8-FF, which may sit
//   inside SysEx) before F7: the partial message is discarded;
// - timed out: no byte for `timeoutNanos` while a message is open.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/midi/sysex/SysexQueue.h"

namespace adx::midi {

struct SysexStats {
    std::uint64_t completed{0};
    std::uint64_t oversize{0};
    std::uint64_t interrupted{0};
    std::uint64_t timedOut{0};
};

class SysexAssembler {
public:
    static constexpr std::int64_t kDefaultTimeoutNanos = 2'000'000'000;

    explicit SysexAssembler(SysexQueue& queue,
                            std::int64_t timeoutNanos = kDefaultTimeoutNanos) noexcept
        : m_queue(queue), m_timeout(timeoutNanos) {}

    /// DEVICE THREAD. Bytes that arrived at `wallNanos`: a whole message, a piece of
    /// one, or a short message that interrupts one. Returns how many bytes it took as
    /// SysEx (0 for a short message it does not own, which the caller then handles).
    std::size_t feed(std::int64_t wallNanos, std::span<const std::uint8_t> bytes) noexcept;

    /// DEVICE THREAD. Drops a message left open for longer than the timeout.
    void poll(std::int64_t wallNanos) noexcept;

    /// Whether a message is open (an F0 seen, no F7 yet).
    [[nodiscard]] bool active() const noexcept {
        return m_active;
    }
    [[nodiscard]] const SysexStats& stats() const noexcept {
        return m_stats;
    }

private:
    void finish() noexcept;

    SysexQueue& m_queue;
    std::int64_t m_timeout;
    std::array<std::uint8_t, SysexMessage::kMaxBytes> m_buffer{};
    std::size_t m_size{0};
    bool m_active{false};
    bool m_overflow{false};
    std::int64_t m_last{0};
    SysexStats m_stats;
};

} // namespace adx::midi
