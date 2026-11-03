// adx-thread: realtime
//
// The audio thread's whole part in recording (phase_8.md §3.1): copy the input into a
// lock-free ring. It never touches a file, never allocates, never waits.
//
// Stereo always: a mono input is written to both sides, an input of more than two
// channels contributes its first two. The arrangement position of the first frame is
// published with it, because where a take belongs is decided from where it started.
//
// An overrun - the writer fell behind and the ring is full - is counted and *ends the
// take* at that frame. A recording with a silent gap in it is worse than one that
// stopped, because the gap is easy to miss and impossible to fix afterwards.
#pragma once

#include <atomic>
#include <cstdint>

#include "engine/rt/SpscRing.h"

namespace adx::record {

/// ~5.5 s of stereo at 96 kHz - 4 MB, so an InputCapture lives on the heap.
inline constexpr std::size_t kCaptureRingSamples = std::size_t{1} << 20;

class InputCapture {
public:
    /// AUDIO THREAD. `in` is `frames` interleaved frames of `channels` channels, or null
    /// for a device with no input (captured as silence). `position` is the arrangement
    /// sample of the block's first frame; `rolling` whether the transport is playing.
    /// Captures only while armed and rolling.
    void capture(const float* in, std::uint32_t frames, std::uint32_t channels,
                 std::int64_t position, bool rolling) noexcept;

    /// Main thread: begin capturing on the next rolling block, or stop.
    void arm(bool armed) noexcept {
        m_armed.store(armed, std::memory_order_release);
    }

    /// Writer thread. Up to `max` samples (interleaved stereo) out of the ring, always
    /// a whole number of frames.
    std::size_t drain(float* out, std::size_t max) noexcept;

    /// The arrangement sample of the first captured frame, once there is one. Any
    /// thread; the acquire pairs with the release that precedes the first push.
    [[nodiscard]] bool started() const noexcept {
        return m_started.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::int64_t firstPosition() const noexcept {
        return m_firstFrameAt.load(std::memory_order_acquire);
    }
    /// Frames pushed since arming.
    [[nodiscard]] std::uint64_t framesCaptured() const noexcept {
        return m_frames.load(std::memory_order_acquire);
    }
    /// Samples the ring had no room for. Non-zero means the take ended early.
    [[nodiscard]] std::uint64_t overrunCount() const noexcept {
        return m_overruns.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool overran() const noexcept {
        return overrunCount() > 0;
    }

private:
    rt::SpscRing<float, kCaptureRingSamples> m_ring;
    std::atomic<bool> m_armed{false};
    std::atomic<bool> m_started{false};
    std::atomic<std::int64_t> m_firstFrameAt{0};
    std::atomic<std::uint64_t> m_frames{0};
    std::atomic<std::uint64_t> m_overruns{0};
};

} // namespace adx::record
