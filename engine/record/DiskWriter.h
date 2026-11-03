// adx-thread: main
//
// The writer thread (phase_8.md §3.1): drains an InputCapture's ring into a streaming
// 32-bit float WAV, through a preallocated buffer, with the header rewritten every
// second so a crash mid-take still leaves a playable file.
//
// It also decides which captured frames belong to the take. Captured frame i was
// played back to the performer at arrangement sample firstPosition + i - latency - the
// latency being the round trip it took to reach the input - and only frames whose
// compensated position falls inside [from, to) are written. So the file's first frame
// *is* arrangement sample `from`, which RecordSession puts on a tick: the take lands
// where it was heard to the sample, although a tick is several samples long.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "engine/render/WavEncoder.h"

namespace adx::record {

class InputCapture;

struct WriterWindow {
    /// Arrangement samples. `to` open-ended by default.
    std::int64_t from{0};
    std::int64_t to{std::numeric_limits<std::int64_t>::max()};
    /// Samples between a sound leaving the output and arriving at the input, plus the
    /// graph's own latency (Monitoring.h).
    std::int64_t latency{0};
};

class DiskWriter {
public:
    DiskWriter(InputCapture& capture, std::filesystem::path path, std::uint32_t sampleRate,
               WriterWindow window);
    ~DiskWriter();
    DiskWriter(const DiskWriter&) = delete;
    DiskWriter& operator=(const DiskWriter&) = delete;
    DiskWriter(DiskWriter&&) = delete;
    DiskWriter& operator=(DiskWriter&&) = delete;

    /// Creates the file and starts the thread.
    bool start(std::string& error);
    /// Drains what is left, finishes the file, joins. Idempotent.
    bool stop(std::string& error);

    [[nodiscard]] std::uint64_t framesWritten() const noexcept {
        return m_written.load(std::memory_order_acquire);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return m_path;
    }

    /// Tests: stop draining, as a slow disk would, so the ring fills.
    void setStalled(bool stalled) noexcept {
        m_stalled.store(stalled, std::memory_order_release);
    }

private:
    void run();
    void consume(std::size_t samples);

    InputCapture& m_capture;
    std::filesystem::path m_path;
    std::uint32_t m_sampleRate;
    WriterWindow m_window;
    render::WavEncoder m_encoder;
    std::vector<float> m_buffer;
    std::vector<float> m_kept;
    std::uint64_t m_consumedFrames{0};
    std::uint64_t m_sinceCheckpoint{0};
    std::atomic<std::uint64_t> m_written{0};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_stalled{false};
    std::string m_error;
    std::thread m_thread;
};

} // namespace adx::record
