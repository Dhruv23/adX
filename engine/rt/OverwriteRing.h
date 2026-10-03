// A ring the writer never blocks on and never dequeues from: it overwrites the
// oldest data and readers take whatever the newest window holds.
//
// Direct port of _archive/src-cpp/include/AudioEngine.h:182-209 (`AudioTap`), whose
// memory ordering was already correct, generalised from one hard-coded stereo tap to
// N typed instances. This is the right shape for meters, scopes and spectrum
// displays: the audio thread must never wait for a UI thread that is busy, and a UI
// that misses a frame at 60 Hz has lost nothing worth having.
//
// Contrast with SpscRing, which is for events that must not be lost.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "engine/rt/RtConfig.h"

namespace adx::rt {

#if defined(_MSC_VER)
#    pragma warning(push)
// C4324: "structure was padded due to alignment specifier". The padding is the
// entire purpose of the alignas below - it is what keeps the producer's counter,
// the consumer's counter and the data off each other's cache lines. Warning about
// it is like warning that a lock is slow: true, intended, and not actionable.
#    pragma warning(disable : 4324)
#endif

template<class Frame, std::size_t Capacity> class OverwriteRing {
    static_assert(std::is_trivially_copyable_v<Frame>);
    static_assert(std::is_default_constructible_v<Frame>);
    static_assert(std::has_single_bit(Capacity), "Capacity must be a power of two");

public:
    using value_type = Frame;

    /// Audio thread only. Never allocates, never blocks, never fails.
    ADX_RT_HOT void write(const Frame& frame) noexcept {
        const std::uint64_t cursor = m_writeCursor.load(std::memory_order_relaxed);
        m_buffer[cursor & kMask] = frame;
        m_writeCursor.fetch_add(1, std::memory_order_release);
    }

    /// Any reader thread. Copies up to `count` of the most recently written frames
    /// into `out`, oldest first, and returns how many were actually available -
    /// fewer than `count` only before the ring has filled once.
    ///
    /// A reader can be overtaken mid-copy and see a seam. That is inherent to a
    /// non-blocking writer and is why this type is named for what it does.
    [[nodiscard]] std::size_t readLatest(Frame* out, std::size_t count) const noexcept {
        count = std::min(count, Capacity);
        const std::uint64_t writePos = m_writeCursor.load(std::memory_order_acquire);
        const auto available =
            static_cast<std::size_t>(std::min(static_cast<std::uint64_t>(count), writePos));
        for (std::size_t i = 0; i < available; ++i) {
            const std::uint64_t framesAgo = available - i;
            out[i] = m_buffer[(writePos - framesAgo) & kMask];
        }
        return available;
    }

    /// Total frames ever written. The archived version kept this in a size_t and
    /// computed `available = min(count, writePos)`, which is correct but quietly
    /// assumes the cursor never wraps. The assumption is kept, and here is the
    /// arithmetic behind it: at 48 kHz a uint64_t cursor wraps in about 12 million
    /// years.
    [[nodiscard]] std::uint64_t framesWritten() const noexcept {
        return m_writeCursor.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    alignas(kCacheLine) std::atomic<std::uint64_t> m_writeCursor{0};
    alignas(kCacheLine) std::array<Frame, Capacity> m_buffer{};
};

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

/// Stereo sample pair. The scope and spectrum tap.
struct StereoFrame {
    float left{};
    float right{};
};

/// One meter reading. Peak and RMS are computed on the audio thread, where the
/// samples already are, rather than shipping every sample to the UI to be squared.
struct LevelFrame {
    float peakLeft{};
    float peakRight{};
    float rmsLeft{};
    float rmsRight{};
    /// BS.1770-4 loudness in LUFS (phase_4.md §4.10); -200 is silence.
    float momentary{-200.0F};
    float shortTerm{-200.0F};
    float integrated{-200.0F};
    /// 4x-oversampled peak of either channel, linear. Zero on strips that do not
    /// measure it: true peak is the master's (and the limiter's) - not free.
    float truePeak{};
};

} // namespace adx::rt
