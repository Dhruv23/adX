// adx-thread: main
#include "engine/geometry/AnalyzerHub.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

#include "engine/dsp/Fft.h"

namespace adx::geometry {

namespace {

/// The worker's pass: often enough that a 60 Hz display always has a fresh spectrum.
constexpr auto kPass = std::chrono::milliseconds(8);
/// Spectrogram rows use at most this FFT: more resolution than columns is wasted.
constexpr std::uint32_t kSpectrogramFft = 4096;

std::uint32_t clampFft(std::uint32_t size) noexcept {
    return std::bit_floor(std::clamp<std::uint32_t>(size, 256, 32768));
}

} // namespace

AnalyzerHub::AnalyzerHub(const ScopeTap& tap, std::uint32_t sampleRate)
    : m_tap(tap), m_sampleRate(sampleRate == 0 ? 48000 : sampleRate), m_history(kHistory),
      m_cursor(tap.framesWritten()), m_drain(ScopeTap::capacity()) {
    m_spectrogram.configure(m_settings.spectrogramRows, m_settings.spectrogramColumns,
                            m_settings.spectrumStyle.floorDb);
    m_spectrogramShown.configure(m_settings.spectrogramRows, m_settings.spectrogramColumns,
                                 m_settings.spectrumStyle.floorDb);
    m_recent.resize(kHistory / 4);
    m_thread = std::thread([this] { run(); });
}

AnalyzerHub::~AnalyzerHub() {
    {
        const std::scoped_lock lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

AnalyzerSettings AnalyzerHub::settings() const {
    const std::scoped_lock lock(m_mutex);
    return m_settings;
}

void AnalyzerHub::configure(const AnalyzerSettings& settings) {
    AnalyzerSettings next = settings;
    next.fftSize = clampFft(settings.fftSize);
    next.spectrogramRows = std::clamp<std::uint32_t>(settings.spectrogramRows, 16, 2048);
    next.spectrogramColumns = std::clamp<std::uint32_t>(settings.spectrogramColumns, 16, 4096);
    const std::scoped_lock lock(m_mutex);
    if (next.spectrogramRows != m_settings.spectrogramRows ||
        next.spectrogramColumns != m_settings.spectrogramColumns) {
        m_spectrogram.configure(next.spectrogramRows, next.spectrogramColumns,
                                next.spectrumStyle.floorDb);
        m_spectrogramShown.configure(next.spectrogramRows, next.spectrogramColumns,
                                     next.spectrumStyle.floorDb);
    }
    m_settings = next;
}

void AnalyzerHub::drain() {
    // By absolute position from where the last pass stopped, so the history has no
    // seams however the audio thread's writes fall between passes.
    while (true) {
        std::uint64_t lost = 0;
        const std::size_t got = m_tap.readFrom(m_cursor, m_drain.data(), m_drain.size(), lost);
        if (lost > 0) {
            m_lost.fetch_add(lost, std::memory_order_relaxed);
        }
        m_cursor += lost + got;
        for (std::size_t i = 0; i < got; ++i) {
            m_history[static_cast<std::size_t>(m_historyWritten % kHistory)] = m_drain[i];
            ++m_historyWritten;
        }
        if (got < m_drain.size()) {
            return;
        }
    }
}

void AnalyzerHub::transform(std::uint32_t size, dsp::WindowKind window, std::vector<float>& db) {
    if (m_windowSize != size || m_windowKind != window) {
        m_windowTable.resize(size);
        for (std::uint32_t i = 0; i < size; ++i) {
            m_windowTable[i] = static_cast<float>(dsp::windowValue(window, i, size));
        }
        m_windowSize = size;
        m_windowKind = window;
    }
    static thread_local dsp::Fft fft;
    static thread_local std::vector<dsp::Complex> spectrum;
    if (fft.size() != size) {
        fft.prepare(size);
        spectrum.resize(size);
    }
    const double gain = dsp::windowCoherentGain(window, size);
    const std::uint64_t end = m_historyWritten;
    const std::uint64_t start = end >= size ? end - size : 0;
    for (std::uint32_t i = 0; i < size; ++i) {
        const std::uint64_t at = start + i;
        float mono = 0.0F;
        if (at < end) {
            const rt::StereoFrame& f = m_history[static_cast<std::size_t>(at % kHistory)];
            mono = 0.5F * (f.left + f.right);
        }
        spectrum[i] = dsp::Complex(mono * m_windowTable[i], 0.0F);
    }
    fft.forward(spectrum);
    db.resize((size / 2) + 1);
    // A full-scale sine reads 0 dB: magnitude over N/2, over the window's gain.
    const double scale = 2.0 / (static_cast<double>(size) * std::max(gain, 1e-9));
    for (std::size_t k = 0; k < db.size(); ++k) {
        const double magnitude = std::abs(spectrum[k]) * scale;
        db[k] = static_cast<float>(20.0 * std::log10(std::max(magnitude, 1e-10)));
    }
    m_transforms.fetch_add(1, std::memory_order_relaxed);
}

void AnalyzerHub::run() {
    auto last = std::chrono::steady_clock::now();
    while (true) {
        AnalyzerSettings settings;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait_for(lock, kPass, [this] { return m_stop; });
            if (m_stop) {
                return;
            }
            settings = m_settings;
        }
        const std::uint64_t before = m_historyWritten;
        drain();
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - last).count();
        last = now;
        // Only over a full window: a part-filled one reads the level low.
        if (settings.spectrum && m_historyWritten > before &&
            m_historyWritten >= settings.fftSize) {
            transform(settings.fftSize, settings.window, m_spectrumDb);
            const std::scoped_lock lock(m_mutex);
            m_spectrumReady = m_spectrumDb;
            ++m_spectrumSerial;
        }
        if (settings.spectrogram) {
            m_rowDebt += elapsed * settings.spectrogramRate;
            const std::uint32_t rowSize = std::min(settings.fftSize, kSpectrogramFft);
            if (m_historyWritten > before && m_historyWritten >= rowSize && m_rowDebt >= 1.0) {
                // Rows come at the configured rate whatever the pass rate; a long stall
                // pushes one row, not a burst of identical ones.
                m_rowDebt = std::min(m_rowDebt - 1.0, 1.0);
                transform(rowSize, settings.window, m_rowDb);
                const std::scoped_lock lock(m_mutex);
                m_spectrogram.push(m_rowDb, m_sampleRate);
            }
        } else {
            m_rowDebt = 0.0;
        }
    }
}

void AnalyzerHub::frame(const rt::LevelFrame& master, double dtSeconds) {
    AnalyzerSettings settings;
    {
        const std::scoped_lock lock(m_mutex);
        settings = m_settings;
        if (m_spectrumSerial != m_spectrumSeen) {
            m_spectrumShown = m_spectrumReady;
            m_spectrumSeen = m_spectrumSerial;
        }
        if (settings.spectrogram && m_spectrogram.pushed() != m_spectrogramShown.pushed()) {
            m_spectrogramShown = m_spectrogram;
        }
    }
    if (settings.spectrum) {
        m_spectrum.build(m_spectrumShown, m_sampleRate, dtSeconds, settings.spectrumStyle);
    }
    if (settings.scope || settings.vector || settings.particles) {
        // The newest frames straight from the tap: any thread may read it.
        const std::size_t want = std::min<std::size_t>(
            m_recent.size(),
            std::max<std::size_t>({std::size_t{2} * settings.scopeStyle.windowFrames,
                                   settings.vectorStyle.windowFrames, 1024U}));
        const std::size_t got = m_tap.readLatest(m_recent.data(), want);
        const std::span<const rt::StereoFrame> recent(m_recent.data(), got);
        if (settings.scope) {
            m_scope.build(recent, settings.scopeStyle);
        }
        if (settings.vector) {
            m_vector.build(recent, settings.vectorStyle);
        }
        if (settings.particles) {
            m_particles.build(recent, dtSeconds);
        }
    }
    if (settings.loudness) {
        m_loudness.build(master, settings.loudnessStyle);
    }
}

} // namespace adx::geometry
