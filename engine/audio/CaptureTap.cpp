// adx-thread: main
#include "engine/audio/CaptureTap.h"

#include <chrono>

#include "engine/render/Encoders.h"

namespace adx::audio {

namespace {

/// A tenth of the tap at 48 kHz: the worker has 680 ms of slack before a frame is lost.
constexpr auto kPass = std::chrono::milliseconds(5);

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape) - stop() joins a thread; nothing here throws.
CaptureTap::~CaptureTap() {
    if (running()) {
        static_cast<void>(stop());
    }
}

void CaptureTap::start() {
    if (running()) {
        static_cast<void>(stop());
    }
    {
        const std::scoped_lock lock(m_mutex);
        m_capture = Capture{};
        m_cursor = m_tap.framesWritten();
        m_capture.startFrame = m_cursor;
        m_chunk.resize(4096);
    }
    m_frames.store(0, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread([this] {
        while (m_running.load(std::memory_order_acquire)) {
            drain();
            std::this_thread::sleep_for(kPass);
        }
    });
}

void CaptureTap::drain() {
    const std::scoped_lock lock(m_mutex);
    while (true) {
        std::uint64_t lost = 0;
        const std::size_t got = m_tap.readFrom(m_cursor, m_chunk.data(), m_chunk.size(), lost);
        m_capture.lost += lost;
        m_cursor += lost + got;
        for (std::size_t i = 0; i < got; ++i) {
            m_capture.samples.push_back(m_chunk[i].left);
            m_capture.samples.push_back(m_chunk[i].right);
        }
        m_capture.frames += got;
        m_frames.store(m_capture.frames, std::memory_order_relaxed);
        if (got < m_chunk.size()) {
            return;
        }
    }
}

Capture CaptureTap::stop() {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    drain();
    const std::scoped_lock lock(m_mutex);
    return std::move(m_capture);
}

bool writeCaptureWav(const Capture& capture, std::uint32_t sampleRate,
                     const std::filesystem::path& path, std::string& error) {
    auto encoder = render::makeEncoder(render::FileFormat::Wav32f);
    render::EncodeSettings settings;
    settings.format = render::FileFormat::Wav32f;
    settings.sampleRate = sampleRate;
    settings.channels = 2;
    return encoder->open(path, settings, error) && encoder->write(capture.samples, error) &&
           encoder->finish(error);
}

} // namespace adx::audio
