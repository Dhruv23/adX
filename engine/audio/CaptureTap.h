// adx-thread: main
//
// The master bus, captured to memory while it plays (phase_10.5.md §4.5): what an agent
// listens with. The audio thread already copies every output frame into the scope tap
// (CallbackCore::writeTaps), so capturing adds nothing to the callback - a worker drains
// the tap by absolute position (OverwriteRing::readFrom) every few milliseconds and
// counts any frame it was too late for. A capture with `lost() == 0` is every frame the
// device was given, in order (capture_tap_matches_offline).
//
// Main thread to start and stop; the worker is this class's own.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/audio/ScopeTap.h"

namespace adx::audio {

struct Capture {
    /// Interleaved stereo.
    std::vector<float> samples;
    std::uint64_t frames{0};
    /// Frames the worker found already overwritten: 0 for a whole capture.
    std::uint64_t lost{0};
    /// The tap position the capture started at: its first frame's index in the stream.
    std::uint64_t startFrame{0};
};

class CaptureTap {
public:
    explicit CaptureTap(const ScopeTapRing& tap) : m_tap(tap) {}
    ~CaptureTap();
    CaptureTap(const CaptureTap&) = delete;
    CaptureTap& operator=(const CaptureTap&) = delete;
    CaptureTap(CaptureTap&&) = delete;
    CaptureTap& operator=(CaptureTap&&) = delete;

    /// Starts at the next frame the audio thread writes. A capture already running is
    /// stopped and discarded first.
    void start();
    /// Stops after draining what is written by now, and hands the capture over.
    [[nodiscard]] Capture stop();
    [[nodiscard]] bool running() const noexcept {
        return m_running.load(std::memory_order_acquire);
    }
    /// Frames captured so far.
    [[nodiscard]] std::uint64_t frames() const noexcept {
        return m_frames.load(std::memory_order_relaxed);
    }

private:
    void drain();

    const ScopeTapRing& m_tap;
    std::mutex m_mutex;
    Capture m_capture;
    std::uint64_t m_cursor{0};
    std::vector<rt::StereoFrame> m_chunk;
    std::atomic<bool> m_running{false};
    std::atomic<std::uint64_t> m_frames{0};
    std::thread m_thread;
};

/// `capture` as a 32-bit float WAV at `sampleRate`. False with `error` on failure.
[[nodiscard]] bool writeCaptureWav(const Capture& capture, std::uint32_t sampleRate,
                                   const std::filesystem::path& path, std::string& error);

} // namespace adx::audio
