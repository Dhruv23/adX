// adx-thread: realtime
//
// MIDI input (phase_9.md §4.1): a device thread pushes messages into a lock-free ring,
// timestamped on the audio clock; the audio thread drains the ring at the top of each
// block and places every event at its true offset in the block.
//
// The audio clock is what makes that possible. Once a block the audio thread publishes
// where the stream is - the frame its block starts at and the wall-clock time then - and
// the device thread maps a message's arrival time through it. An event is scheduled one
// block after it arrived, because that is the earliest block that has not started yet:
// a constant block of latency, and no jitter, rather than up to a block of jitter.
#pragma once

#include <atomic>
#include <cstdint>
#include <span>

#include "engine/midi/MidiEvent.h"
#include "engine/midi/sysex/SysexAssembler.h"
#include "engine/rt/SpscRing.h"

namespace adx::midi {

/// Where the stream is in wall time. A seqlock: one writer (the audio thread), readers
/// on any thread retry the rare read that overlapped a write.
class AudioClock {
public:
    /// AUDIO THREAD. The block starting at stream frame `frames` began at `wallNanos`.
    void publish(std::uint64_t frames, std::int64_t wallNanos) noexcept;

    /// Any thread. False until the first publish, or after a few reads that each
    /// overlapped a write (the caller then uses its last good reading).
    [[nodiscard]] bool read(std::uint64_t& frames, std::int64_t& wallNanos) const noexcept;

private:
    std::atomic<std::uint64_t> m_sequence{0};
    std::atomic<std::uint64_t> m_frames{0};
    std::atomic<std::int64_t> m_wall{0};
};

/// An event placed in the block being rendered.
struct BlockMidi {
    /// Frames from the block's start.
    std::uint32_t offset{0};
    MidiEvent event;
};

class MidiInput {
public:
    static constexpr std::size_t kRingCapacity = 4096;

    MidiInput(const AudioClock& clock, std::uint32_t sampleRate, std::uint32_t blockFrames,
              std::uint8_t device) noexcept
        : m_clock(clock), m_sampleRate(sampleRate), m_blockFrames(blockFrames), m_device(device) {}

    /// DEVICE THREAD. One complete message (RtMidi delivers them whole) that arrived at
    /// `wallNanos` on the steady clock. SysEx goes to the attached assembler, if any
    /// (phase_11.5.md §4.4), and never into the ring; without one it is dropped, as are
    /// active sensing and anything malformed. A full ring counts the message as lost.
    void onMessage(std::int64_t wallNanos, std::span<const std::uint8_t> bytes) noexcept;

    /// DEVICE THREAD. An event that already knows when it plays (its timestampSamples):
    /// a script, an on-screen keyboard, a test. False when the ring is full.
    bool push(const MidiEvent& event) noexcept;

    /// The stream frame a message arriving at `wallNanos` plays at: one block after the
    /// published block it arrived during, at its offset into that block.
    [[nodiscard]] static std::uint64_t scheduleAt(std::uint64_t blockFrame, std::int64_t blockWall,
                                                  std::int64_t wallNanos, std::uint32_t sampleRate,
                                                  std::uint32_t blockFrames) noexcept;

    /// AUDIO THREAD. Writes the events due in [blockStart, blockStart + frames), each at
    /// its offset, into `out`, oldest first; returns how many. An event due later stays
    /// for its own block. An event already late (its block has passed) plays at offset
    /// 0. More than `out` holds waits for the next block.
    std::size_t drain(std::uint64_t blockStart, std::uint32_t frames,
                      std::span<BlockMidi> out) noexcept;

    /// Messages lost to a full ring. Any thread.
    [[nodiscard]] std::uint64_t lostCount() const noexcept {
        return m_lost.load(std::memory_order_acquire);
    }

    /// Main thread, before the port opens: SysEx from now on goes to `assembler`
    /// (nullptr: dropped). The assembler must outlive the input.
    void attachSysex(SysexAssembler* assembler) noexcept {
        m_sysex = assembler;
    }

private:
    SysexAssembler* m_sysex{nullptr};
    const AudioClock& m_clock;
    std::uint32_t m_sampleRate;
    std::uint32_t m_blockFrames;
    std::uint8_t m_device;
    rt::SpscRing<MidiEvent, kRingCapacity> m_ring;
    std::atomic<std::uint64_t> m_lost{0};
    /// Device thread only: the last clock reading that succeeded.
    std::uint64_t m_lastFrame{0};
    std::int64_t m_lastWall{0};
    /// Audio thread only: an event popped but not due yet.
    MidiEvent m_held{};
    bool m_holding{false};
};

} // namespace adx::midi
