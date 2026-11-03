// The analyzer pipeline (phase_10.md §4.4, §6): each builder on known signals, the
// spectrogram ring and the particle pool bounded, and the hub's FFT kept off the audio
// thread.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

#include "engine/audio/NullBackend.h"
#include "engine/geometry/AnalyzerHub.h"
#include "engine/render/RenderEngine.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

using namespace adx::geometry;
using adx::rt::StereoFrame;

std::vector<StereoFrame> sine(double hz, std::size_t frames, float level = 0.5F,
                              float rightSign = 1.0F, double rate = 48000.0) {
    std::vector<StereoFrame> out(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(
            level * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
        out[i] = {.left = v, .right = rightSign * v};
    }
    return out;
}

/// A tone written into a tap as the audio thread would, from a thread of its own.
class FakeAudio {
public:
    explicit FakeAudio(ScopeTap& tap, double hz) : m_tap(tap), m_hz(hz) {
        m_thread = std::thread([this] {
            std::uint64_t n = 0;
            while (!m_stop.load()) {
                for (int i = 0; i < 480; ++i, ++n) {
                    const auto v =
                        static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * m_hz *
                                                          static_cast<double>(n) / 48000.0));
                    m_tap.write(StereoFrame{.left = v, .right = v});
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }
    ~FakeAudio() {
        m_stop.store(true);
        m_thread.join();
    }
    FakeAudio(const FakeAudio&) = delete;
    FakeAudio& operator=(const FakeAudio&) = delete;
    FakeAudio(FakeAudio&&) = delete;
    FakeAudio& operator=(FakeAudio&&) = delete;

private:
    ScopeTap& m_tap;
    double m_hz;
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace

TEST_CASE("analyzer_hub_spectrum_finds_the_tone", "[geometry][analyzers]") {
    ScopeTap tap;
    AnalyzerHub hub(tap, 48000);
    AnalyzerSettings settings;
    settings.spectrum = true;
    settings.fftSize = 8192;
    hub.configure(settings);
    const FakeAudio audio(tap, 1000.0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (hub.transforms() < 20 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(hub.transforms() >= 20);
    hub.frame({}, 1.0 / 60.0);
    const auto columns = hub.spectrum().columns();
    const auto loudest =
        static_cast<std::size_t>(std::ranges::max_element(columns) - columns.begin());
    const double hzOfColumn =
        20.0 * std::pow(1000.0, (static_cast<double>(loudest) + 0.5) / columns.size());
    CHECK(std::abs(std::log2(hzOfColumn / 1000.0)) < 0.05); // within a semitone
    // A half-scale sine reads about -6 dB.
    CHECK_THAT(columns[loudest], Catch::Matchers::WithinAbs(-6.0, 1.5));
    CHECK(hub.spectrum().fill().vertexCount() > 0);
    CHECK(hub.spectrum().lines().vertexCount() > 0);

    // 32768 points works too, on the worker.
    settings.fftSize = 32768;
    hub.configure(settings);
    const std::uint64_t before = hub.transforms();
    while (hub.transforms() < before + 5 &&
           std::chrono::steady_clock::now() < deadline + std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(hub.transforms() >= before + 5);
}

TEST_CASE("spectrum_peak_hold_falls", "[geometry][analyzers]") {
    SpectrumGeometry spectrum;
    SpectrumStyle style;
    style.columns = 64;
    std::vector<float> loud(2049, -20.0F);
    std::vector<float> quiet(2049, -80.0F);
    spectrum.build(loud, 48000, 0.0, style);
    spectrum.build(quiet, 48000, 1.0, style);
    // A second later the peak has fallen 12 dB from -20, the curve is at -80.
    CHECK_THAT(spectrum.peaks()[32], Catch::Matchers::WithinAbs(-32.0, 1e-3));
    CHECK_THAT(spectrum.columns()[32], Catch::Matchers::WithinAbs(-80.0, 1e-3));
    spectrum.resetPeaks();
    spectrum.build(quiet, 48000, 0.0, style);
    CHECK_THAT(spectrum.peaks()[32], Catch::Matchers::WithinAbs(-80.0, 1e-3));
    CHECK(SpectrumGeometry::xOf(20.0) == 0.0F);
    CHECK_THAT(SpectrumGeometry::xOf(20000.0), Catch::Matchers::WithinAbs(1.0, 1e-6));
}

TEST_CASE("spectrogram_ring_fixed", "[geometry][analyzers]") {
    // No allocation while scrolling, at any window size: the ring and its column map
    // were sized at configure().
    for (const std::uint32_t columns : {64U, 512U, 2048U}) {
        SpectrogramGeometry spectrogram;
        spectrogram.configure(128, columns);
        const std::size_t bytes = spectrogram.pixels().size();
        const std::uint8_t* storage = spectrogram.data();
        std::vector<std::vector<float>> rows;
        for (const std::size_t bins : {129U, 2049U, 16385U}) {
            rows.emplace_back(bins, -40.0F);
        }
        auto& log = adx::rt::ViolationLog::instance();
        const std::uint64_t before = log.count(adx::rt::ViolationKind::Allocation);
        {
            const adx::rt::ScopedRtSection section;
            for (int i = 0; i < 1000; ++i) {
                spectrogram.push(rows[static_cast<std::size_t>(i) % rows.size()], 48000);
            }
        }
        CHECK(log.count(adx::rt::ViolationKind::Allocation) == before);
        CHECK(spectrogram.pixels().size() == bytes);
        CHECK(spectrogram.data() == storage);
        CHECK(spectrogram.pushed() == 1000);
        CHECK(spectrogram.head() == (127 + 1000) % 128);
    }
    // Louder is brighter.
    SpectrogramGeometry s;
    s.configure(4, 8);
    s.push(std::vector<float>(1025, -85.0F), 48000);
    const int dark = s.pixels()[(s.head() * 8 * 4)] + s.pixels()[(s.head() * 8 * 4) + 1];
    s.push(std::vector<float>(1025, -3.0F), 48000);
    const int bright = s.pixels()[(s.head() * 8 * 4)] + s.pixels()[(s.head() * 8 * 4) + 1];
    CHECK(bright > dark + 200);
}

TEST_CASE("particles_pool_bounded", "[geometry][analyzers]") {
    ParticleGeometry particles(7);
    for (int i = 0; i < 100; ++i) {
        particles.burst(1.0F); // 40 a burst: 4000 asked for, 512 kept
    }
    CHECK(particles.alive() == ParticleGeometry::capacity());
    particles.build({}, 1.0 / 60.0);
    CHECK(particles.particles().vertexCount() <= ParticleGeometry::capacity() * 6);
    CHECK(particles.particles().vertexCount() > 0);
    // Transients fire bursts; silence lets them die out.
    ParticleGeometry fresh(7);
    std::vector<StereoFrame> silence(1024);
    const std::vector<StereoFrame> hit = sine(80.0, 1024, 0.8F);
    fresh.build(silence, 1.0 / 60.0);
    CHECK(fresh.bursts() == 0);
    fresh.build(hit, 1.0 / 60.0);
    CHECK(fresh.bursts() == 1);
    fresh.build(hit, 1.0 / 60.0);
    CHECK(fresh.bursts() == 1); // a sustained level is not a new transient
    for (int i = 0; i < 200; ++i) {
        fresh.build(silence, 1.0 / 60.0);
    }
    CHECK(fresh.alive() == 0);
    // Seeded: the same input gives the same particles.
    ParticleGeometry a(3);
    ParticleGeometry b(3);
    a.build(hit, 0.016);
    b.build(hit, 0.016);
    const auto fa = a.particles().floats();
    const auto fb = b.particles().floats();
    CHECK(std::equal(fa.begin(), fa.end(), fb.begin(), fb.end()));
}

TEST_CASE("scope_triggers_on_a_rising_crossing", "[geometry][analyzers]") {
    ScopeGeometry scope;
    ScopeStyle style;
    style.windowFrames = 960;
    // 437 Hz does not divide the window: free-running, the phase would wander.
    const std::vector<StereoFrame> tone = sine(437.0, 4000);
    scope.build(tone, style);
    REQUIRE(scope.triggered());
    const std::size_t start = scope.lastStart();
    CHECK(tone[start - 1].left < 0.0F);
    CHECK(tone[start].left >= 0.0F);
    CHECK(scope.lines().vertexCount() == (960 - 1) * 4);
    // Hold keeps the trace.
    style.hold = true;
    const std::uint64_t revision = scope.lines().revision();
    scope.build(sine(437.0, 4000, 0.1F), style);
    CHECK(scope.lines().revision() == revision);
    // Silence: no crossing, free-running.
    style.hold = false;
    scope.build(std::vector<StereoFrame>(4000), style);
    CHECK_FALSE(scope.triggered());
}

TEST_CASE("vectorscope_correlation", "[geometry][analyzers]") {
    CHECK_THAT(phaseCorrelation(sine(500.0, 4800)), Catch::Matchers::WithinAbs(1.0, 1e-5));
    CHECK_THAT(phaseCorrelation(sine(500.0, 4800, 0.5F, -1.0F)),
               Catch::Matchers::WithinAbs(-1.0, 1e-5));
    CHECK(phaseCorrelation(std::vector<StereoFrame>(100)) == 0.0F);
    // Left and right a quarter period apart: uncorrelated.
    std::vector<StereoFrame> quadrature(4800);
    for (std::size_t i = 0; i < quadrature.size(); ++i) {
        const double t = 2.0 * std::numbers::pi * 500.0 * static_cast<double>(i) / 48000.0;
        quadrature[i] = {.left = static_cast<float>(std::sin(t)),
                         .right = static_cast<float>(std::cos(t))};
    }
    CHECK(std::abs(phaseCorrelation(quadrature)) < 0.01F);
    VectorGeometry vector;
    VectorStyle style;
    style.smoothing = 0.0F;
    vector.build(sine(500.0, 4800), style);
    CHECK_THAT(vector.correlation(), Catch::Matchers::WithinAbs(1.0, 1e-5));
    // Mono draws a vertical line: every x is 0.
    const auto floats = vector.trace().floats();
    for (std::size_t i = 0; i < floats.size(); i += kFloatsPerVertex) {
        CHECK(floats[i] == 0.0F);
    }
    CHECK(vector.meter().vertexCount() == 18);
}

TEST_CASE("loudness_history_scrolls", "[geometry][analyzers]") {
    LoudnessGeometry loudness;
    LoudnessStyle style;
    style.historyFrames = 8;
    adx::rt::LevelFrame frame;
    frame.momentary = -12.0F;
    frame.shortTerm = -14.0F;
    frame.integrated = -16.0F;
    for (int i = 0; i < 20; ++i) {
        loudness.build(frame, style);
    }
    CHECK(loudness.integrated() == -16.0F);
    CHECK(loudness.bars().vertexCount() == 36);
    CHECK(loudness.history().vertexCount() == (8 - 1) * 2); // a full ring of segments
    loudness.reset();
    loudness.build(frame, style);
    CHECK(loudness.history().vertexCount() == 0);
}

TEST_CASE("visualizer_no_audio_thread_work", "[geometry][analyzers][.slow]") {
    // A 32768-point FFT selected: the audio thread's median callback is unchanged
    // within noise, because the transform runs on the hub's worker.
    const adx::project::Project project =
        adx::tests::toneProject({{.start = 0, .length = adx::core::kPpq * 64, .pitch = 69}});
    const auto medianCallbackUs = [&](bool analyzers) {
        auto backend = std::make_unique<adx::audio::NullBackend>();
        adx::render::RenderEngine engine{
            std::move(backend), adx::render::EngineOptions{
                                    .sampleRate = 48000, .blockFrames = 256, .outputChannels = 2}};
        REQUIRE(engine.open().code == adx::audio::Error::Code::None);
        REQUIRE(engine.setProject(project, 0).rebuilt);
        engine.play();
        std::unique_ptr<AnalyzerHub> hub;
        if (analyzers) {
            hub = std::make_unique<AnalyzerHub>(engine.audio().scopeTap(), 48000);
            AnalyzerSettings settings;
            settings.spectrum = true;
            settings.spectrogram = true;
            settings.fftSize = 32768;
            hub->configure(settings);
        }
        REQUIRE(engine.start().code == adx::audio::Error::Code::None);
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            engine.pump();
            if (hub) {
                hub->frame({}, 0.01);
            }
        }
        engine.stop();
        std::vector<adx::audio::CallbackTiming> timings(1024);
        const std::size_t got =
            engine.audio().callbacks().timingTap().readLatest(timings.data(), timings.size());
        std::vector<double> us;
        for (std::size_t i = 0; i < got; ++i) {
            us.push_back(timings[i].durationNs / 1000.0);
        }
        std::ranges::nth_element(us, us.begin() + static_cast<std::ptrdiff_t>(us.size() / 2));
        if (hub) {
            CHECK(hub->transforms() > 50);
        }
        return us[us.size() / 2];
    };
    const double without = medianCallbackUs(false);
    const double with = medianCallbackUs(true);
    INFO("median callback " << without << " us without the analyzers, " << with
                            << " us with a 32k FFT");
    CHECK(with < (without * 1.5) + 20.0);
}
