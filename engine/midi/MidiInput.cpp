// adx-thread: realtime
#include "engine/midi/MidiInput.h"

#include <algorithm>
#include <cmath>

namespace adx::midi {

void AudioClock::publish(std::uint64_t frames, std::int64_t wallNanos) noexcept {
    // Odd while writing. The fences order the two payload stores between the two
    // sequence stores for any reader that sees the same sequence before and after.
    const std::uint64_t sequence = m_sequence.load(std::memory_order_relaxed);
    m_sequence.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    m_frames.store(frames, std::memory_order_relaxed);
    m_wall.store(wallNanos, std::memory_order_relaxed);
    m_sequence.store(sequence + 2, std::memory_order_release);
}

bool AudioClock::read(std::uint64_t& frames, std::int64_t& wallNanos) const noexcept {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint64_t before = m_sequence.load(std::memory_order_acquire);
        if (before == 0) {
            return false;
        }
        if ((before & 1U) != 0) {
            continue;
        }
        const std::uint64_t f = m_frames.load(std::memory_order_relaxed);
        const std::int64_t w = m_wall.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m_sequence.load(std::memory_order_relaxed) == before) {
            frames = f;
            wallNanos = w;
            return true;
        }
    }
    return false;
}

std::uint64_t MidiInput::scheduleAt(std::uint64_t blockFrame, std::int64_t blockWall,
                                    std::int64_t wallNanos, std::uint32_t sampleRate,
                                    std::uint32_t blockFrames) noexcept {
    const std::int64_t since = std::max<std::int64_t>(0, wallNanos - blockWall);
    const auto elapsed = static_cast<std::uint64_t>(
        std::llround(static_cast<double>(since) * static_cast<double>(sampleRate) / 1e9));
    return blockFrame + blockFrames + elapsed;
}

void MidiInput::onMessage(std::int64_t wallNanos, std::span<const std::uint8_t> bytes) noexcept {
    if (m_sysex != nullptr && m_sysex->feed(wallNanos, bytes) > 0) {
        return; // SysEx, or a piece of it: the main thread's, never a channel's
    }
    if (bytes.empty() || bytes.size() > 3 || (bytes[0] & 0x80U) == 0) {
        return;
    }
    const std::uint8_t status = bytes[0];
    // SysEx and its end, active sensing and reset carry nothing a channel plays.
    if (status == 0xF0 || status == 0xF7 || status == 0xFE || status == 0xFF) {
        return;
    }
    std::uint64_t frame = 0;
    std::int64_t wall = 0;
    if (m_clock.read(frame, wall)) {
        m_lastFrame = frame;
        m_lastWall = wall;
    }
    MidiEvent const event{
        .timestampSamples =
            scheduleAt(m_lastFrame, m_lastWall, wallNanos, m_sampleRate, m_blockFrames),
        .status = status,
        .data1 = bytes.size() > 1 ? static_cast<std::uint8_t>(bytes[1] & 0x7FU) : std::uint8_t{0},
        .data2 = bytes.size() > 2 ? static_cast<std::uint8_t>(bytes[2] & 0x7FU) : std::uint8_t{0},
        .device = m_device};
    if (!m_ring.tryPush(event)) {
        m_lost.fetch_add(1, std::memory_order_acq_rel);
    }
}

bool MidiInput::push(const MidiEvent& event) noexcept {
    if (!m_ring.tryPush(event)) {
        m_lost.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
    return true;
}

std::size_t MidiInput::drain(std::uint64_t blockStart, std::uint32_t frames,
                             std::span<BlockMidi> out) noexcept {
    const std::uint64_t blockEnd = blockStart + frames;
    std::size_t written = 0;
    while (written < out.size()) {
        if (!m_holding) {
            if (!m_ring.tryPop(m_held)) {
                break;
            }
            m_holding = true;
        }
        if (m_held.timestampSamples >= blockEnd) {
            break; // due in a later block; keep it
        }
        const std::uint64_t at = std::max(m_held.timestampSamples, blockStart);
        out[written++] =
            BlockMidi{.offset = static_cast<std::uint32_t>(at - blockStart), .event = m_held};
        m_holding = false;
    }
    return written;
}

} // namespace adx::midi
