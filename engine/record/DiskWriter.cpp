// adx-thread: main
#include "engine/record/DiskWriter.h"

#include <chrono>

#include "engine/record/InputCapture.h"

namespace adx::record {

namespace {

/// Samples drained per pass: a third of a second of stereo at 96 kHz.
constexpr std::size_t kPassSamples = 65536;

} // namespace

DiskWriter::DiskWriter(InputCapture& capture, std::filesystem::path path, std::uint32_t sampleRate,
                       WriterWindow window)
    : m_capture(capture), m_path(std::move(path)), m_sampleRate(sampleRate), m_window(window) {
    m_buffer.resize(kPassSamples);
    m_kept.reserve(kPassSamples);
}

DiskWriter::~DiskWriter() {
    // A destructor that threw would end the program; a take that cannot be finished
    // here has already been reported by stop() or never started.
    try {
        std::string ignored;
        static_cast<void>(stop(ignored));
    } catch (...) { // NOLINT(bugprone-empty-catch) - nothing left to tell
    }
}

bool DiskWriter::start(std::string& error) {
    std::error_code ignored;
    std::filesystem::create_directories(m_path.parent_path(), ignored);
    render::EncodeSettings settings;
    settings.format = render::FileFormat::Wav32f;
    settings.sampleRate = m_sampleRate;
    settings.channels = 2;
    if (!m_encoder.open(m_path, settings, error)) {
        return false;
    }
    m_thread = std::thread([this] { run(); });
    return true;
}

void DiskWriter::consume(std::size_t samples) {
    // Keep only the frames whose compensated position falls in the window.
    const std::int64_t first = m_capture.firstPosition() - m_window.latency;
    m_kept.clear();
    for (std::size_t s = 0; s + 1 < samples; s += 2) {
        const std::int64_t at = first + static_cast<std::int64_t>(m_consumedFrames);
        ++m_consumedFrames;
        if (at >= m_window.from && at < m_window.to) {
            m_kept.push_back(m_buffer[s]);
            m_kept.push_back(m_buffer[s + 1]);
        }
    }
    if (m_kept.empty()) {
        return;
    }
    if (!m_encoder.write(m_kept, m_error)) {
        return;
    }
    m_written.fetch_add(m_kept.size() / 2, std::memory_order_acq_rel);
    m_sinceCheckpoint += m_kept.size() / 2;
    if (m_sinceCheckpoint >= m_sampleRate) {
        m_sinceCheckpoint = 0;
        static_cast<void>(m_encoder.checkpoint(m_error));
    }
}

void DiskWriter::run() {
    while (true) {
        const bool stopping = m_stopping.load(std::memory_order_acquire);
        if (m_stalled.load(std::memory_order_acquire) && !stopping) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const std::size_t got = m_capture.drain(m_buffer.data(), m_buffer.size());
        if (got > 0) {
            consume(got);
            continue;
        }
        if (stopping) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool DiskWriter::stop(std::string& error) {
    if (!m_thread.joinable()) {
        return m_error.empty();
    }
    m_stopping.store(true, std::memory_order_release);
    m_thread.join();
    std::string finish;
    const bool ok = m_encoder.finish(finish) && m_error.empty();
    error = !m_error.empty() ? m_error : finish;
    return ok;
}

} // namespace adx::record
