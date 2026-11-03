// adx-thread: main
//
// The six analyzers behind one call a frame (phase_10.md §4.4):
//
//   audio thread -> the master scope tap (an OverwriteRing, CallbackCore)
//                -> a worker thread: drains the tap, keeps 64k frames of history,
//                   runs the FFT at the chosen size and window, pushes spectrogram rows
//                -> frame(), once per UI frame on the main thread: takes the worker's
//                   newest spectrum, reads the newest frames for the scope, vectorscope
//                   and particles, and builds every enabled analyzer's geometry
//
// So a 32768-point FFT runs on neither the audio thread nor the UI thread
// (visualizer_no_audio_thread_work), and six open panels cost one engine call a frame
// between them (visualizer_one_ffi_call): the binding builds them all inside the
// frame call the meters already use.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "engine/audio/ScopeTap.h"
#include "engine/dsp/Window.h"
#include "engine/geometry/LoudnessGeometry.h"
#include "engine/geometry/ParticleGeometry.h"
#include "engine/geometry/ScopeGeometry.h"
#include "engine/geometry/SpectrogramGeometry.h"
#include "engine/geometry/SpectrumGeometry.h"
#include "engine/geometry/VectorGeometry.h"
#include "engine/rt/OverwriteRing.h"

namespace adx::geometry {

using ScopeTap = audio::ScopeTapRing;

/// Which analyzers are open, and how each is set.
struct AnalyzerSettings {
    bool spectrum{false};
    bool spectrogram{false};
    bool scope{false};
    bool vector{false};
    bool loudness{false};
    bool particles{false};

    /// 256 .. 32768, a power of two.
    std::uint32_t fftSize{4096};
    dsp::WindowKind window{dsp::WindowKind::Hann};
    SpectrumStyle spectrumStyle;
    std::uint32_t spectrogramRows{256};
    std::uint32_t spectrogramColumns{512};
    /// Spectrogram rows a second.
    double spectrogramRate{60.0};
    ScopeStyle scopeStyle;
    VectorStyle vectorStyle;
    LoudnessStyle loudnessStyle;
};

class AnalyzerHub {
public:
    AnalyzerHub(const ScopeTap& tap, std::uint32_t sampleRate);
    ~AnalyzerHub();
    AnalyzerHub(const AnalyzerHub&) = delete;
    AnalyzerHub& operator=(const AnalyzerHub&) = delete;
    AnalyzerHub(AnalyzerHub&&) = delete;
    AnalyzerHub& operator=(AnalyzerHub&&) = delete;

    void configure(const AnalyzerSettings& settings);
    [[nodiscard]] AnalyzerSettings settings() const;

    /// Main thread, once per UI frame: builds every enabled analyzer.
    void frame(const rt::LevelFrame& master, double dtSeconds);

    [[nodiscard]] SpectrumGeometry& spectrum() noexcept {
        return m_spectrum;
    }
    [[nodiscard]] ScopeGeometry& scope() noexcept {
        return m_scope;
    }
    [[nodiscard]] VectorGeometry& vector() noexcept {
        return m_vector;
    }
    [[nodiscard]] LoudnessGeometry& loudness() noexcept {
        return m_loudness;
    }
    [[nodiscard]] ParticleGeometry& particles() noexcept {
        return m_particles;
    }
    /// The image the worker writes; read only inside frame()'s lock, or through
    /// copySpectrogram().
    [[nodiscard]] const SpectrogramGeometry& spectrogram() const noexcept {
        return m_spectrogramShown;
    }

    /// FFTs the worker has finished; frames it found already overwritten in the tap.
    [[nodiscard]] std::uint64_t transforms() const noexcept {
        return m_transforms.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t framesLost() const noexcept {
        return m_lost.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint32_t sampleRate() const noexcept {
        return m_sampleRate;
    }

private:
    void run();
    void drain();
    void transform(std::uint32_t size, dsp::WindowKind window, std::vector<float>& db);

    const ScopeTap& m_tap;
    std::uint32_t m_sampleRate;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    AnalyzerSettings m_settings;
    bool m_stop{false};

    // Worker side.
    static constexpr std::size_t kHistory = 65536;
    std::vector<rt::StereoFrame> m_history;
    std::uint64_t m_cursor{0};
    std::vector<rt::StereoFrame> m_drain;
    std::uint64_t m_historyWritten{0};
    std::vector<float> m_work;
    std::vector<float> m_windowTable;
    std::uint32_t m_windowSize{0};
    dsp::WindowKind m_windowKind{dsp::WindowKind::Hann};
    std::vector<float> m_spectrumDb;
    std::vector<float> m_rowDb;
    double m_rowDebt{0.0};
    SpectrogramGeometry m_spectrogram;

    // Shared, under m_mutex.
    std::vector<float> m_spectrumReady;
    std::uint64_t m_spectrumSerial{0};

    // Main side.
    std::vector<float> m_spectrumShown;
    std::uint64_t m_spectrumSeen{0};
    SpectrogramGeometry m_spectrogramShown;
    std::vector<rt::StereoFrame> m_recent;
    SpectrumGeometry m_spectrum;
    ScopeGeometry m_scope;
    VectorGeometry m_vector;
    LoudnessGeometry m_loudness;
    ParticleGeometry m_particles;

    std::atomic<std::uint64_t> m_transforms{0};
    std::atomic<std::uint64_t> m_lost{0};
    std::thread m_thread;
};

} // namespace adx::geometry
