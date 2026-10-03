// The Tranche A gate (phase_4.md §4.3, §6): suffocation.adx through the v1 shim and the
// real instruments and effects, within 1.5 dB of the archived engine in every one of
// 31 third-octave bands.
//
// Not a bit-identity claim - the filter topology, voice handling and sample rate all
// legitimately changed - and the spectral distance is what the plan asks for instead.
// The reference is committed as band levels rather than the 30 MB render they come
// from (tests/data/reference/suffocation_v1_bands.txt says how it was made; a
// human A/B, recorded in plans/STATE.md, is the other half of the gate).
//
// The measurement is tools/ab/bands.py's, reproduced exactly so the two agree to the
// hundredth of a dB: the mono sum, a symmetric Hann window of 65536 points stepped by
// half its length, the power spectrum averaged over frames and normalised to mean
// square, then summed over each band [c * 2^-1/6, c * 2^1/6).
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include "engine/dsp/Fft.h"
#include "engine/format/adx/Parser.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/Corpus.h"

namespace {

constexpr std::size_t kFftSize = 65536;
constexpr std::size_t kBands = 31;
constexpr double kToleranceDb = 1.5;

struct Band {
    double centre{0.0};
    double level{0.0};
};

std::vector<Band> readReference() {
    std::ifstream in(adx::tests::repoRoot() / "tests" / "data" / "reference" /
                     "suffocation_v1_bands.txt");
    std::vector<Band> bands;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line.front() == '#') {
            continue;
        }
        std::istringstream fields(line);
        Band band;
        fields >> band.centre >> band.level;
        bands.push_back(band);
    }
    return bands;
}

std::vector<double> bandLevels(const std::vector<float>& interleaved, std::uint32_t sampleRate,
                               const std::vector<Band>& reference) {
    const std::size_t frames = interleaved.size() / 2;
    std::vector<double> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        mono[i] = (static_cast<double>(interleaved[2 * i]) + interleaved[(2 * i) + 1]) * 0.5;
    }

    // numpy.hanning: symmetric, 0.5 - 0.5 cos(2 pi n / (N - 1)).
    std::vector<double> window(kFftSize);
    double windowPower = 0.0;
    for (std::size_t n = 0; n < kFftSize; ++n) {
        window[n] = 0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                          static_cast<double>(kFftSize - 1)));
        windowPower += window[n] * window[n];
    }

    adx::dsp::Fft fft;
    fft.prepare(kFftSize);
    std::vector<adx::dsp::Complex> work(kFftSize);
    std::vector<double> power((kFftSize / 2) + 1, 0.0);
    std::size_t count = 0;
    for (std::size_t start = 0; start + kFftSize < frames; start += kFftSize / 2) {
        for (std::size_t n = 0; n < kFftSize; ++n) {
            work[n] = adx::dsp::Complex{static_cast<float>(mono[start + n] * window[n]), 0.0F};
        }
        fft.forward(work);
        for (std::size_t k = 0; k < power.size(); ++k) {
            const double re = work[k].real();
            const double im = work[k].imag();
            power[k] += (re * re) + (im * im);
        }
        ++count;
    }
    REQUIRE(count > 0);
    const double scale =
        2.0 / (static_cast<double>(count) * static_cast<double>(kFftSize) * windowPower);

    std::vector<double> levels;
    const double binHz = static_cast<double>(sampleRate) / static_cast<double>(kFftSize);
    for (const Band& band : reference) {
        const double low = band.centre * std::pow(2.0, -1.0 / 6.0);
        const double high = band.centre * std::pow(2.0, 1.0 / 6.0);
        double sum = 0.0;
        for (std::size_t k = 0; k < power.size(); ++k) {
            const double f = static_cast<double>(k) * binHz;
            if (f >= low && f < high) {
                sum += power[k] * scale;
            }
        }
        levels.push_back(10.0 * std::log10(sum + 1e-30));
    }
    return levels;
}

} // namespace

TEST_CASE("suffocation_spectral_match", "[render][gate][v1]") {
    const std::vector<Band> reference = readReference();
    REQUIRE(reference.size() == kBands);

    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(
        adx::tests::readFile(adx::tests::repoRoot() / "docs" / "examples" / "suffocation.adx"),
        project, stack, diagnostics);
    REQUIRE_FALSE(diagnostics.hasErrors());

    adx::render::OfflineRenderOptions options;
    options.tailSeconds = 4.0; // as the reference was rendered
    adx::render::RenderStats stats;
    const std::vector<float> samples = adx::render::renderOffline(project, options, stats);
    REQUIRE(stats.error.empty());

    const std::vector<double> levels = bandLevels(samples, options.sampleRate, reference);
    std::size_t over = 0;
    for (std::size_t b = 0; b < kBands; ++b) {
        const double delta = levels[b] - reference[b].level;
        INFO(reference[b].centre << " Hz: reference " << reference[b].level << " dB, engine "
                                 << levels[b] << " dB, delta " << delta);
        CHECK(std::abs(delta) <= kToleranceDb);
        over += std::abs(delta) > kToleranceDb ? 1 : 0;
    }
    CHECK(over == 0);
}
