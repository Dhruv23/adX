// Melody extraction (phase_10.md §6): basic-pitch's note creation on hand-built
// activations, which needs no model; the ONNX build's accuracy on a synthesized,
// labelled corpus; and the flag-off build's promise that the extractor is absent.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <numbers>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/analysis/AnalysisJob.h"
#include "engine/analysis/AudioInput.h"
#include "engine/analysis/Chroma.h"
#include "engine/analysis/MelodyExtractor.h"
#include "engine/analysis/NoteCreation.h"

namespace {

using namespace adx::analysis;

Activations empty(std::uint32_t frames) {
    Activations a;
    a.frames = frames;
    a.note.assign(static_cast<std::size_t>(frames) * kPitchBins, 0.0F);
    a.onset = a.note;
    return a;
}

void set(std::vector<float>& grid, std::uint32_t frame, std::uint32_t bin, float v) {
    grid[(static_cast<std::size_t>(frame) * kPitchBins) + bin] = v;
}

} // namespace

TEST_CASE("note_creation_from_activations", "[analysis]") {
    Activations a = empty(400);
    // Middle C (bin 39): a struck note, frames 20-80, with an onset.
    for (std::uint32_t f = 20; f < 80; ++f) {
        set(a.note, f, 39, 0.8F);
        set(a.note, f, 40, 0.35F); // bleed into the neighbour: claimed with it
    }
    set(a.onset, 20, 39, 0.9F);
    // G4 (bin 46): fades in with no onset at all; only the melodia pass finds it.
    for (std::uint32_t f = 120; f < 220; ++f) {
        set(a.note, f, 46, std::min(0.7F, 0.02F * static_cast<float>(f - 119)));
    }
    // A blip shorter than 11 frames is not a note.
    for (std::uint32_t f = 300; f < 306; ++f) {
        set(a.note, f, 30, 0.9F);
    }
    set(a.onset, 300, 30, 0.9F);
    // A gap of 5 quiet frames inside a note is tolerated: one note, not two.
    for (std::uint32_t f = 320; f < 390; ++f) {
        set(a.note, f, 50, (f >= 350 && f < 355) ? 0.1F : 0.6F);
    }
    set(a.onset, 320, 50, 0.8F);

    const std::vector<DetectedNote> notes = notesFromActivations(a);
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].pitch == 60);
    CHECK_THAT(notes[0].startSeconds, Catch::Matchers::WithinAbs(20.0 / kActivationFps, 1e-9));
    CHECK_THAT(notes[0].seconds,
               Catch::Matchers::WithinAbs(60.0 / kActivationFps, 2.0 / kActivationFps));
    CHECK(notes[0].velocity == static_cast<std::uint8_t>(0.8F * 127.0F));
    CHECK(notes[1].pitch == 67);
    CHECK(notes[1].startSeconds > 120.0 / kActivationFps);
    CHECK(notes[2].pitch == 71);
    CHECK_THAT(notes[2].seconds,
               Catch::Matchers::WithinAbs(70.0 / kActivationFps, 2.0 / kActivationFps));

    // Without the melodia pass, the fade-in is lost.
    NoteCreationOptions strict;
    strict.melodiaTrick = false;
    strict.inferOnsets = false;
    CHECK(notesFromActivations(a, strict).size() == 2);
    // Pure: the same input gives the same output.
    const std::vector<DetectedNote> again = notesFromActivations(a);
    REQUIRE(again.size() == notes.size());
    for (std::size_t i = 0; i < notes.size(); ++i) {
        CHECK(again[i].startSeconds == notes[i].startSeconds);
        CHECK(again[i].pitch == notes[i].pitch);
    }
    // A malformed grid is no notes, not a crash.
    Activations bad = empty(10);
    bad.onset.pop_back();
    CHECK(notesFromActivations(bad).empty());
}

TEST_CASE("melody_extractor_onnx_off", "[analysis]") {
    // The flag decides whether the extractor exists at all; the rest of the suite
    // passing in a flag-off build (every per-push CI job) is the other half.
#if defined(ADX_ENABLE_ONNX)
    CHECK(melodyExtractorAvailable());
#else
    CHECK_FALSE(melodyExtractorAvailable());
#endif
}

TEST_CASE("resample_mono_keeps_a_tone", "[analysis]") {
    std::vector<float> tone(48000);
    for (std::size_t i = 0; i < tone.size(); ++i) {
        tone[i] = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(i) / 48000.0));
    }
    const std::vector<float> out = resampleMono(tone, 48000, 22050);
    CHECK(out.size() == 22050);
    // Rising zero crossings over the middle second: 440 Hz still.
    int crossings = 0;
    for (std::size_t i = 1001; i < 21001; ++i) {
        if (out[i - 1] < 0.0F && out[i] >= 0.0F) {
            ++crossings;
        }
    }
    CHECK(std::abs(crossings - 399) <= 1); // 20000 samples at 22050 Hz is 0.907 s
    CHECK(resampleMono(tone, 48000, 48000) == tone);
}

#if defined(ADX_ENABLE_ONNX)

TEST_CASE("melody_extractor_accuracy", "[analysis][onnx]") {
    // A labelled corpus by construction: monophonic and two-voice lines of piano-like
    // tones (decaying partials) at 22050 Hz, note F1 with basic-pitch's own matching
    // rule (onset within 50 ms, same pitch).
    constexpr std::uint32_t kRate = kMelodySampleRate;
    std::mt19937 random(7);
    struct Truth {
        double start;
        double seconds;
        int pitch;
    };
    std::vector<Truth> truth;
    double t = 0.25;
    while (t < 28.0) {
        const double seconds = 0.25 + (0.125 * static_cast<double>(random() % 5));
        truth.push_back({t, seconds, 55 + static_cast<int>(random() % 24)});
        if (random() % 3 == 0) {
            truth.push_back({t, seconds, 43 + static_cast<int>(random() % 12)});
        }
        t += seconds + 0.05;
    }
    std::vector<float> audio(static_cast<std::size_t>(30.0 * kRate), 0.0F);
    for (const Truth& n : truth) {
        const double hz = 440.0 * std::exp2((n.pitch - 69) / 12.0);
        const auto start = static_cast<std::size_t>(n.start * kRate);
        const auto length = static_cast<std::size_t>((n.seconds + 0.3) * kRate);
        for (int k = 1; k <= 8 && hz * k < kRate / 2.0; ++k) {
            const double amplitude = 0.2 / std::pow(k, 1.3);
            for (std::size_t i = 0; i < length && start + i < audio.size(); ++i) {
                const double s = static_cast<double>(i) / kRate;
                const double gate = s < n.seconds ? 1.0 : std::exp(-(s - n.seconds) * 30.0);
                const double envelope =
                    std::min(1.0, s * 400.0) * std::exp(-s * (1.5 + k * 0.4)) * gate;
                audio[start + i] += static_cast<float>(
                    amplitude * envelope * std::sin(2.0 * std::numbers::pi * hz * k * s));
            }
        }
    }
    const auto detected = extractMelody(audio);
    REQUIRE(detected.has_value());
    std::vector<bool> used(detected->size(), false);
    int hits = 0;
    for (const Truth& n : truth) {
        for (std::size_t i = 0; i < detected->size(); ++i) {
            const DetectedNote& d = (*detected)[i];
            if (!used[i] && d.pitch == n.pitch && std::abs(d.startSeconds - n.start) <= 0.05) {
                used[i] = true;
                ++hits;
                break;
            }
        }
    }
    const double precision = static_cast<double>(hits) / static_cast<double>(detected->size());
    const double recall = static_cast<double>(hits) / static_cast<double>(truth.size());
    const double f1 = 2.0 * precision * recall / (precision + recall);
    INFO("notes " << truth.size() << " detected " << detected->size() << " precision " << precision
                  << " recall " << recall << " F1 " << f1);
    CHECK(f1 > 0.8);
    // Cancelling stops it.
    CHECK_FALSE(
        extractMelody(audio, defaultMelodyModel(), [](double) { return false; }).has_value());
}

#endif

TEST_CASE("analysis_jobs_cancellable", "[analysis]") {
    using Clock = std::chrono::steady_clock;
    // Ten minutes of noise at 22050 Hz: a chromagram long enough to still be running
    // when the cancel arrives.
    std::vector<float> noise(static_cast<std::size_t>(22050) * 600);
    std::mt19937 random(3);
    std::uniform_real_distribution<float> sample(-0.5F, 0.5F);
    for (float& s : noise) {
        s = sample(random);
    }
    std::size_t framesDone = 0;
    adx::analysis::AnalysisJob job([&](adx::analysis::AnalysisControl& control) {
        adx::analysis::ChromaOptions options;
        options.progress = [&control](double f) { return control.proceed(f); };
        framesDone = adx::analysis::chromagram(noise, 22050, options).frames.size();
    });
    while (job.progress() <= 0.0F && !job.done()) {
        std::this_thread::yield();
    }
    const auto asked = Clock::now();
    job.cancel();
    job.wait();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - asked);
    CHECK(took.count() < 200);
    CHECK(job.cancelled());
    CHECK(job.done());
    CHECK(framesDone < noise.size() / 4096);

    // An exception in the work is the job's error, not a crash.
    adx::analysis::AnalysisJob failing(
        [](adx::analysis::AnalysisControl&) { throw std::runtime_error("bad input"); });
    failing.wait();
    CHECK(failing.error() == "bad input");
    CHECK(failing.done());
}
