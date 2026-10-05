// Tranche C's effects and Overdrive (phase_4.md §4.9, §6). The contract every effect
// shares - click-free bypass, the wet/dry law, clone independence, mix 0 is dry, no
// allocation, golden hashes - is test_effect_base.cpp and test_golden_units.cpp, which
// pick these up from the catalog. These are what each one is for.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "engine/core/TempoMap.h"
#include "engine/dsp/FormantBank.h"
#include "engine/dsp/Oversampler.h"
#include "engine/effects/Drive.h"
#include "engine/effects/Factory.h"
#include "engine/effects/GrossBeat.h"
#include "engine/project/Project.h"
#include "engine/project/TypeCatalog.h"
#include "engine/render/OfflineRender.h"
#include "engine/rt/BlockArena.h"
#include "engine/transport/TimeSource.h"
#include "tests/cpp/dsp/DspTestUtil.h"
#include "tests/cpp/effects/EffectHarness.h"

using adx::tests::EffectRun;
using adx::tests::kTestRate;
using adx::tests::preparedEffect;
using adx::tests::runEffect;
using adx::tests::slotParamIndex;
using adx::tests::slotParams;
using adx::tests::StereoSignal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

std::vector<float> params(std::string_view type,
                          std::initializer_list<std::pair<std::string_view, float>> values) {
    std::vector<float> out = slotParams(type);
    for (const auto& [name, value] : values) {
        const std::uint32_t index = slotParamIndex(type, name);
        REQUIRE(index != adx::project::kNoParam);
        out[index] = value;
    }
    return out;
}

std::vector<float> window(const std::vector<float>& x, std::size_t from, std::size_t length) {
    return {x.begin() + static_cast<std::ptrdiff_t>(from),
            x.begin() + static_cast<std::ptrdiff_t>(from + length)};
}

double amplitudeAt(const std::vector<float>& x, double hz, std::size_t from = 9600) {
    return adx::tests::toneAmplitude(window(x, from, 32768), hz, kTestRate);
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * x[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

std::size_t peakIndex(const std::vector<float>& x) {
    std::size_t peak = 0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (std::abs(x[i]) > std::abs(x[peak])) {
            peak = i;
        }
    }
    return peak;
}

float largestStep(const std::vector<float>& x, std::size_t from, std::size_t to) {
    float step = 0.0F;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < to; ++i) {
        step = std::max(step, std::abs(x[i] - x[i - 1]));
    }
    return step;
}

} // namespace

// --- Oversampler, Saturation, Overdrive ----------------------------------------------------

TEST_CASE("oversampler_latency_is_declared", "[effects][dsp]") {
    adx::dsp::prepareOversampler();
    adx::dsp::Oversampler4x os;
    std::vector<float> out(512);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = os.process(i == 100 ? 1.0F : 0.0F, [](float s) { return s; });
    }
    CHECK(peakIndex(out) == 100 + adx::dsp::kOversampleLatency);
    CHECK_THAT(out[100 + adx::dsp::kOversampleLatency], WithinAbs(1.0, 0.05));
    // A sine in the audio band passes at unity.
    adx::dsp::Oversampler4x tone;
    std::vector<float> sine(32768 + 4800);
    for (std::size_t i = 0; i < sine.size(); ++i) {
        sine[i] = tone.process(static_cast<float>(std::sin(2.0 * std::numbers::pi * 10000.0 *
                                                           static_cast<double>(i) / kTestRate)),
                               [](float s) { return s; });
    }
    CHECK_THAT(amplitudeAt(sine, 10000.0, 4800), WithinAbs(1.0, 0.01));
}

TEST_CASE("effect_declares_latency_fixed", "[effects]") {
    // Effects whose latency is a fixed structure: an impulse small enough to stay in
    // the linear region comes out exactly the declared latency later.
    constexpr std::size_t kAt = 3000;
    for (const char* type : {"Saturation", "SpectralFreeze", "Convolution"}) {
        INFO(type);
        const auto node = preparedEffect(type);
        const std::uint32_t declared = node->latencySamples();
        CHECK(declared > 0);
        if (std::string_view(type) == "Convolution") {
            // A room's first reflection is 10 ms in: check against an impulse IR below.
            continue;
        }
        const EffectRun run =
            runEffect(*node, adx::tests::impulse(kAt, 0.01F, 16384), slotParams(type));
        CHECK(peakIndex(run.out.left) == kAt + declared);
    }
}

TEST_CASE("overdrive_declares_latency", "[effects][overdrive]") {
    // The tone and tightness filters smear an impulse, so the lag is read where the
    // output correlates best with the input.
    const StereoSignal in = adx::tests::noise(11U, 0.02F, 24000);
    const auto node = preparedEffect("Overdrive");
    const EffectRun run =
        runEffect(*node, in,
                  params("Overdrive", {{"tone", 12000.0F}, {"tightness", 20.0F}, {"drive", 0.0F}}));
    std::size_t best = 0;
    double bestScore = -1.0;
    for (std::size_t lag = 0; lag < 100; ++lag) {
        double score = 0.0;
        for (std::size_t i = 2000; i < 20000; ++i) {
            score += static_cast<double>(run.out.left[i + lag]) * in.left[i];
        }
        if (score > bestScore) {
            bestScore = score;
            best = lag;
        }
    }
    CHECK(node->latencySamples() == adx::dsp::kOversampleLatency);
    // The 12 kHz tone filter's own group delay is under a sample.
    CHECK(best >= node->latencySamples());
    CHECK(best <= node->latencySamples() + 1);
}

TEST_CASE("overdrive_alias_below_-60dB", "[effects][overdrive]") {
    // phase_4.md §4.9: the oversampled clipper does not alias. Soft and tube, driven
    // 12 dB into the shaper, at 1 kHz and 5 kHz: everything that is not a harmonic of
    // the input is 60 dB below the fundamental.
    for (const float mode : {0.0F, 1.0F}) {
        for (const double hz : {1000.0, 5000.0}) {
            INFO("mode " << mode << " at " << hz << " Hz");
            const StereoSignal in = adx::tests::sine(hz, 0.5F, 48000);
            const EffectRun run = runEffect(
                *preparedEffect("Overdrive"), in,
                params(
                    "Overdrive",
                    {{"mode", mode}, {"drive", 12.0F}, {"tone", 12000.0F}, {"tightness", 20.0F}}));
            CHECK(adx::tests::aliasingDbc(window(run.out.left, 9600, 32768), hz, kTestRate) <
                  -60.0);
        }
    }
    // Hard clipping has 1/n harmonics; with ADAA on top of 4x it is measured, not gated.
    const StereoSignal in = adx::tests::sine(1000.0, 0.5F, 48000);
    const EffectRun hard =
        runEffect(*preparedEffect("Overdrive"), in,
                  params("Overdrive", {{"mode", 2.0F}, {"drive", 12.0F}, {"tone", 12000.0F}}));
    const double aliasing =
        adx::tests::aliasingDbc(window(hard.out.left, 9600, 32768), 1000.0, kTestRate);
    WARN("hard clip at 1 kHz, 12 dB drive: aliasing " << aliasing << " dBc");
    CHECK(aliasing < -40.0);
}

TEST_CASE("overdrive_mode_switch_no_click", "[effects][overdrive]") {
    const StereoSignal in = adx::tests::sine(200.0, 0.5F, 48000);
    const auto toggled = [](std::uint64_t frame, std::vector<float>& p) {
        p[slotParamIndex("Overdrive", "mode")] = frame >= 24000 ? 2.0F : 0.0F;
    };
    const EffectRun run = runEffect(*preparedEffect("Overdrive"), in,
                                    params("Overdrive", {{"drive", 24.0F}}), 64, toggled);
    const float steady = largestStep(run.out.left, 12000, 23000);
    CHECK(largestStep(run.out.left, 23990, 24600) <= steady + 0.01F);
}

TEST_CASE("saturation_models_have_their_harmonics", "[effects][saturation]") {
    // The tube is asymmetric: even harmonics. Tape and transformer are odd only.
    const StereoSignal in = adx::tests::sine(500.0, 0.5F, 48000);
    for (const float model : {0.0F, 1.0F, 2.0F}) {
        INFO("model " << model);
        const EffectRun run = runEffect(*preparedEffect("Saturation"), in,
                                        params("Saturation", {{"model", model}, {"drive", 12.0F}}));
        const double fundamental = amplitudeAt(run.out.left, 500.0);
        const double second = amplitudeAt(run.out.left, 1000.0);
        const double third = amplitudeAt(run.out.left, 1500.0);
        CHECK(third > 1e-3 * fundamental);
        if (model == 0.0F) {
            CHECK(second > 1e-2 * fundamental);
        } else {
            CHECK(second < 1e-3 * fundamental);
        }
    }
}

// --- Modulation -------------------------------------------------------------------------

TEST_CASE("flanger_taps_land_at_the_delay", "[effects][modulation]") {
    // phase_4.md §4.1: impulse in, the taps land at the exact expected sample. Depth 0,
    // no feedback, a 1 ms delay: the input now and again 48 frames later, each halved.
    const EffectRun run =
        runEffect(*preparedEffect("Flanger"), adx::tests::impulse(1000, 1.0F, 4096),
                  params("Flanger", {{"depth", 0.0F}, {"delay", 1.0F}, {"feedback", 0.0F}}));
    CHECK(run.out.left[1000] == 0.5F);
    CHECK(run.out.left[1048] == 0.5F);
    CHECK(run.out.left[1047] == 0.0F);
    CHECK(run.out.left[1049] == 0.0F);
}

TEST_CASE("phaser_notches_at_its_centre", "[effects][modulation]") {
    // Two first-order allpasses at 1 kHz shift 1 kHz by 180 degrees: added to the dry
    // signal, it cancels. Far below, they barely shift: it passes.
    const auto run = [](double hz) {
        return runEffect(
            *preparedEffect("Phaser"), adx::tests::sine(hz, 0.5F, 48000),
            params("Phaser",
                   {{"depth", 0.0F}, {"stages", 1.0F}, {"centre", 1000.0F}, {"feedback", 0.0F}}));
    };
    CHECK(amplitudeAt(run(1000.0).out.left, 1000.0) < 0.005);
    CHECK(amplitudeAt(run(50.0).out.left, 50.0) > 0.45);
}

TEST_CASE("tremolo_depth_sets_the_swing", "[effects][modulation]") {
    const StereoSignal dc{.left = std::vector<float>(48000, 1.0F),
                          .right = std::vector<float>(48000, 1.0F)};
    const EffectRun run = runEffect(*preparedEffect("Tremolo"), dc,
                                    params("Tremolo", {{"depth", 0.5F}, {"rate", 4.0F}}));
    const auto [low, high] = std::ranges::minmax(run.out.left);
    CHECK_THAT(high, WithinAbs(1.0, 1e-3));
    CHECK_THAT(low, WithinAbs(0.5, 1e-3));
}

TEST_CASE("ringmod_leaves_sum_and_difference", "[effects][modulation]") {
    const EffectRun run =
        runEffect(*preparedEffect("RingMod"), adx::tests::sine(1000.0, 0.5F, 48000),
                  params("RingMod", {{"frequency", 300.0F}}));
    CHECK_THAT(amplitudeAt(run.out.left, 700.0), WithinRel(0.25, 0.02));
    CHECK_THAT(amplitudeAt(run.out.left, 1300.0), WithinRel(0.25, 0.02));
    CHECK(amplitudeAt(run.out.left, 1000.0) < 1e-3);
}

TEST_CASE("frequency_shifter_moves_every_partial", "[effects][modulation]") {
    // A true shift: 440 + 100 Hz, and its mirror image at 340 is 30 dB down.
    const EffectRun run =
        runEffect(*preparedEffect("FrequencyShifter"), adx::tests::sine(440.0, 0.5F, 48000),
                  params("FrequencyShifter", {{"shift", 100.0F}}));
    const double shifted = amplitudeAt(run.out.left, 540.0);
    CHECK_THAT(shifted, WithinRel(0.5, 0.02));
    CHECK(amplitudeAt(run.out.left, 340.0) < 0.03 * shifted);
    CHECK(amplitudeAt(run.out.left, 440.0) < 0.03 * shifted);
}

TEST_CASE("stereo_imager_width", "[effects][modulation]") {
    StereoSignal in = adx::tests::noise(4U, 0.3F, 24000);
    const StereoSignal other = adx::tests::noise(5U, 0.3F, 24000);
    in.right = other.right;
    const EffectRun unity =
        runEffect(*preparedEffect("StereoImager"), in, slotParams("StereoImager"));
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE_THAT(unity.out.left[i], WithinAbs(in.left[i], 1e-6));
    }
    const EffectRun mono = runEffect(*preparedEffect("StereoImager"), in,
                                     params("StereoImager", {{"width", 0.0F}, {"lowWidth", 0.0F}}));
    for (std::size_t i = 0; i < in.size(); ++i) {
        REQUIRE(mono.out.left[i] == mono.out.right[i]);
    }
}

// --- Dynamics ---------------------------------------------------------------------------

TEST_CASE("multiband_flat_when_neutral", "[effects][multiband]") {
    // Every band at ratio 1 and 0 dB: the crossovers and their allpass compensation sum
    // to a flat magnitude, +-0.1 dB from 20 Hz to 20 kHz, at every band count.
    for (const float bands : {3.0F, 4.0F, 6.0F}) {
        INFO(bands << " bands");
        std::vector<float> p = params("MultibandComp", {{"bands", bands}});
        for (int b = 1; b <= 6; ++b) {
            p[slotParamIndex("MultibandComp", "band" + std::to_string(b) + ".ratio")] = 1.0F;
        }
        const EffectRun run =
            runEffect(*preparedEffect("MultibandComp"), adx::tests::impulse(0, 1.0F, 65536), p);
        const std::vector<double> power = adx::tests::powerSpectrum(run.out.left);
        // powerSpectrum windows its input; an impulse at 0 sits at the window's foot, so
        // read the response through a rectangular FFT instead.
        adx::dsp::Fft fft;
        fft.prepare(65536);
        std::vector<adx::dsp::Complex> x(65536);
        for (std::size_t i = 0; i < x.size(); ++i) {
            x[i] = adx::dsp::Complex{run.out.left[i], 0.0F};
        }
        fft.forward(x);
        for (std::size_t k = 28; k < 27307; k += 7) { // 20 Hz .. 20 kHz
            const double db = 20.0 * std::log10(std::abs(x[k]));
            REQUIRE_THAT(db, WithinAbs(0.0, 0.1));
        }
        static_cast<void>(power);
    }
}

TEST_CASE("multiband_compresses_only_the_loud_band", "[effects][multiband]") {
    // A loud 60 Hz tone and a quiet 5 kHz one; the lowest band compresses hard.
    StereoSignal in = adx::tests::sine(60.0, 0.9F, 48000);
    const StereoSignal top = adx::tests::sine(5000.0, 0.05F, 48000);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in.left[i] += top.left[i];
        in.right[i] += top.right[i];
    }
    const EffectRun run =
        runEffect(*preparedEffect("MultibandComp"), in,
                  params("MultibandComp", {{"band1.threshold", -30.0F}, {"band1.ratio", 10.0F}}));
    CHECK(amplitudeAt(run.out.left, 60.0) < 0.3);
    CHECK_THAT(amplitudeAt(run.out.left, 5000.0), WithinRel(0.05, 0.05));
}

TEST_CASE("transient_shaper_moves_the_attack", "[effects][transient]") {
    // Bursts that start hard and decay: +attack raises the first milliseconds against
    // the tail, -attack lowers them.
    StereoSignal in = adx::tests::sine(200.0, 0.0F, 48000);
    for (std::size_t i = 0; i < in.size(); ++i) {
        const double t = static_cast<double>(i % 12000) / kTestRate;
        const auto s = static_cast<float>(0.5 * std::exp(-t * 20.0) *
                                          std::sin(2.0 * std::numbers::pi * 200.0 * t));
        in.left[i] = s;
        in.right[i] = s;
    }
    const auto ratio = [&](float attack) {
        const EffectRun run = runEffect(*preparedEffect("TransientShaper"), in,
                                        params("TransientShaper", {{"attack", attack}}));
        return rms(run.out.left, 24000, 24480) / rms(run.out.left, 26400, 28800);
    };
    const double neutral = ratio(0.0F);
    CHECK(ratio(1.0F) > neutral * 1.5);
    CHECK(ratio(-1.0F) < neutral / 1.5);
}

// --- Convolution --------------------------------------------------------------------------

TEST_CASE("convolution_matches_direct", "[effects][convolution]") {
    // phase_4.md §6: FFT convolution matches naive time-domain convolution within 1e-5,
    // for a 1 s IR - here a file, through the slot's `ir=`.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "adx_tranche_c";
    std::filesystem::create_directories(dir);
    std::vector<float> ir(48000);
    adx::dsp::WhiteNoise noise{99U};
    for (std::size_t n = 0; n < ir.size(); ++n) {
        ir[n] =
            noise.next() * static_cast<float>(std::exp(-static_cast<double>(n) / 8000.0)) * 0.05F;
    }
    REQUIRE(adx::render::writeWavFloat32((dir / "ir.wav").string(), ir, 1, 48000));

    adx::project::Project project;
    project.resources.baseDirectory = dir.string();
    adx::project::SampleRef ref;
    ref.id = project.newSampleId();
    ref.path = "ir.wav";
    project.resources.samples.push_back(ref);
    adx::project::Slot slot;
    slot.type = "Convolution";
    slot.impulse = ref.id;
    const auto node = adx::effects::makeEffect(slot, &project.resources);
    node->prepare(adx::graph::PrepareInfo{.sampleRate = kTestRate, .maxBlockFrames = 2048});
    CHECK(adx::effects::configMatches(*node, slot, &project.resources));
    adx::project::Slot room = slot;
    room.impulse = {};
    CHECK_FALSE(adx::effects::configMatches(*node, room, &project.resources));

    const StereoSignal in = adx::tests::noise(3U, 0.5F, 2048 + 512);
    const EffectRun run = runEffect(*node, in, slotParams("Convolution"));
    const std::uint32_t latency = node->latencySamples();
    for (std::size_t n = 0; n < 2048; ++n) {
        double direct = 0.0;
        for (std::size_t k = 0; k <= n; ++k) {
            direct += static_cast<double>(ir[k]) * in.left[n - k];
        }
        REQUIRE_THAT(run.out.left[n + latency], WithinAbs(direct, 1e-5));
    }
}

// --- Spectral -----------------------------------------------------------------------------

TEST_CASE("pitch_shifter_moves_a_sine", "[effects][spectral]") {
    for (const auto& [semitones, target] :
         {std::pair{12.0F, 880.0}, std::pair{-12.0F, 220.0}, std::pair{7.0F, 659.255}}) {
        INFO(semitones << " semitones");
        const EffectRun run =
            runEffect(*preparedEffect("PitchShifter"), adx::tests::sine(440.0, 0.5F, 48000),
                      params("PitchShifter", {{"semitones", semitones}}));
        // A phase vocoder smears a moved partial over its neighbours: most of the
        // energy is at the target, and the original is gone.
        const double moved = amplitudeAt(run.out.left, target);
        CHECK(moved > 0.15);
        CHECK(amplitudeAt(run.out.left, 440.0) < 0.1 * moved);
    }
}

TEST_CASE("spectral_freeze_holds_the_spectrum", "[effects][spectral]") {
    // A 440 Hz tone for half a second, then silence; frozen during the tone, the
    // output keeps sounding 440 Hz after the input has stopped.
    StereoSignal in = adx::tests::sine(440.0, 0.5F, 96000);
    std::fill(in.left.begin() + 24000, in.left.end(), 0.0F);
    std::fill(in.right.begin() + 24000, in.right.end(), 0.0F);
    const auto frozen = [](std::uint64_t frame, std::vector<float>& p) {
        p[slotParamIndex("SpectralFreeze", "freeze")] = frame >= 12000 ? 1.0F : 0.0F;
    };
    const EffectRun run =
        runEffect(*preparedEffect("SpectralFreeze"), in, slotParams("SpectralFreeze"), 64, frozen);
    CHECK(amplitudeAt(run.out.left, 440.0, 48000) > 0.1);
    const EffectRun open =
        runEffect(*preparedEffect("SpectralFreeze"), in, slotParams("SpectralFreeze"));
    CHECK(rms(open.out.left, 48000, 96000) < 1e-6);
}

// --- Vocal --------------------------------------------------------------------------------

TEST_CASE("vocoder_flat_carrier_reproduces_envelope", "[effects][vocoder]") {
    // Noise gated on and off at 4 Hz as the modulator, noise as the carrier: the output
    // follows the modulator's envelope.
    StereoSignal in = adx::tests::noise(8U, 0.4F, 48000);
    for (std::size_t i = 0; i < in.size(); ++i) {
        const bool on = (i / 6000) % 2 == 0;
        in.left[i] *= on ? 1.0F : 0.0F;
        in.right[i] *= on ? 1.0F : 0.0F;
    }
    const EffectRun run =
        runEffect(*preparedEffect("Vocoder"), in,
                  params("Vocoder", {{"carrier", 2.0F}, {"sibilance", 0.0F}, {"release", 10.0F}}));
    std::vector<double> envIn;
    std::vector<double> envOut;
    for (std::size_t from = 0; from + 480 <= in.size(); from += 480) {
        envIn.push_back(rms(in.left, from, from + 480));
        envOut.push_back(rms(run.out.left, from, from + 480));
    }
    const auto mean = [](const std::vector<double>& v) {
        double s = 0.0;
        for (const double x : v) {
            s += x;
        }
        return s / static_cast<double>(v.size());
    };
    const double mi = mean(envIn);
    const double mo = mean(envOut);
    double cov = 0.0;
    double vi = 0.0;
    double vo = 0.0;
    for (std::size_t k = 0; k < envIn.size(); ++k) {
        cov += (envIn[k] - mi) * (envOut[k] - mo);
        vi += (envIn[k] - mi) * (envIn[k] - mi);
        vo += (envOut[k] - mo) * (envOut[k] - mo);
    }
    CHECK(cov / std::sqrt(vi * vo) > 0.9);
}

TEST_CASE("vocoder_sibilance_passthrough", "[effects][vocoder]") {
    // The sidechain carrier, silent: only the sibilance path can make sound.
    const StereoSignal in = adx::tests::noise(9U, 0.4F, 24000);
    const EffectRun with = runEffect(*preparedEffect("Vocoder"), in,
                                     params("Vocoder", {{"carrier", 0.0F}, {"sibilance", 1.0F}}));
    const EffectRun without =
        runEffect(*preparedEffect("Vocoder"), in,
                  params("Vocoder", {{"carrier", 0.0F}, {"sibilance", 0.0F}}));
    CHECK(rms(with.out.left, 4800, 24000) > 0.05);
    CHECK(rms(without.out.left, 4800, 24000) < 1e-9);
}

TEST_CASE("formant_filter_shapes_by_vowel", "[effects][formant]") {
    // Noise through 'a' and through 'i': each is louder than the other at its own first
    // formant - 'a' high and open, 'i' low and closed.
    const StereoSignal in = adx::tests::noise(12U, 0.3F, 48000);
    const auto energy = [&](float vowel, double hz) {
        const EffectRun run = runEffect(*preparedEffect("FormantFilter"), in,
                                        params("FormantFilter", {{"vowelA", vowel}}));
        return amplitudeAt(run.out.left, hz);
    };
    const double f1a = adx::dsp::vowelSpec(adx::dsp::VowelSet::Tenor5, 0).bands[0].frequency;
    const double f1i = adx::dsp::vowelSpec(adx::dsp::VowelSet::Tenor5, 2).bands[0].frequency;
    CHECK(energy(0.0F, f1a) > 2.0 * energy(2.0F, f1a));
    CHECK(energy(2.0F, f1i) > 2.0 * energy(0.0F, f1i));
}

// --- GrossBeat ------------------------------------------------------------------------------

namespace {

/// Runs `node` against a time source rolling from tick 0 at `bpm`, in `block`-frame
/// blocks: what the scheduler does, minus the graph.
StereoSignal runRolling(adx::graph::SlotNode& node, const StereoSignal& in, std::vector<float> slot,
                        double bpm, std::uint32_t block = 64) {
    adx::core::TempoMap tempo;
    tempo.setTempo(adx::core::Ticks{0}, bpm, false);
    adx::transport::TimeSource time;
    time.bindTempo(tempo.view(), kTestRate);
    time.requestState(adx::transport::PlayState::Playing);
    StereoSignal out{.left = std::vector<float>(in.size()), .right = std::vector<float>(in.size())};
    std::vector<std::byte> storage(1U << 20U);
    adx::rt::BlockArena arena{storage.data(), storage.size()};
    for (std::size_t start = 0; start < in.size(); start += block) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::size_t>(block, in.size() - start));
        static_cast<void>(time.beginBlock());
        const std::array<std::span<const float>, 2> inputs{
            std::span<const float>{in.left}.subspan(start, count),
            std::span<const float>{in.right}.subspan(start, count)};
        const std::array<std::span<float>, 2> outputs{
            std::span<float>{out.left}.subspan(start, count),
            std::span<float>{out.right}.subspan(start, count)};
        arena.reset();
        adx::graph::ProcessContext context{.time = time,
                                           .outputs = outputs,
                                           .inputs =
                                               std::span<const std::span<const float>>{inputs},
                                           .frames = count,
                                           .sampleRate = kTestRate,
                                           .events = {},
                                           .params = slot,
                                           .automation = {},
                                           .arena = arena};
        node.process(context);
        static_cast<void>(time.advance(count));
    }
    return out;
}

} // namespace

TEST_CASE("grossbeat_half_speed_reads_half_as_far", "[effects][grossbeat]") {
    // A ramp names its own frame. At 120 BPM a 2-beat cycle is one second; at half speed
    // frame n of each cycle plays frame n / 2 of it.
    StereoSignal ramp{.left = std::vector<float>(96000), .right = std::vector<float>(96000)};
    for (std::size_t i = 0; i < ramp.size(); ++i) {
        ramp.left[i] = static_cast<float>(i) / 100000.0F;
        ramp.right[i] = ramp.left[i];
    }
    const auto node = preparedEffect("GrossBeat");
    const StereoSignal out =
        runRolling(*node, ramp, params("GrossBeat", {{"pattern", 1.0F}, {"length", 1.0F}}), 120.0);
    for (const std::size_t n : {1000U, 20000U, 40000U}) {
        const std::size_t at = 48000 + n; // the second cycle, past the first's start fade
        // Half speed: frame n of the cycle plays frame n / 2 (n is even, so exactly).
        const std::size_t played = 48000 + (n / 2U);
        CHECK_THAT(out.left[at], WithinAbs(static_cast<float>(played) / 100000.0F, 2e-5));
    }
}

TEST_CASE("grossbeat_off_is_the_identity_and_block_size_independent", "[effects][grossbeat]") {
    const StereoSignal in = adx::tests::noise(21U, 0.4F, 48000);
    const auto identity =
        runRolling(*preparedEffect("GrossBeat"), in, slotParams("GrossBeat"), 128.0);
    CHECK(identity.left == in.left);
    const auto p = params("GrossBeat", {{"pattern", 3.0F}, {"gate", 2.0F}});
    const auto a = runRolling(*preparedEffect("GrossBeat"), in, p, 128.0, 64);
    const auto b = runRolling(*preparedEffect("GrossBeat"), in, p, 128.0, 1024);
    CHECK(a.left == b.left);
}

TEST_CASE("grossbeat_pattern_is_causal", "[effects][grossbeat]") {
    for (std::uint8_t pattern = 0; pattern < adx::effects::kGrossBeatPatterns; ++pattern) {
        INFO("pattern " << pattern);
        for (double p = 0.0; p < 1.0; p += 0.001) {
            const double f =
                adx::effects::grossBeatMap(static_cast<adx::effects::GrossBeatPattern>(pattern), p);
            REQUIRE(f <= p + 1e-12);
            REQUIRE(f >= 0.0);
        }
    }
}
