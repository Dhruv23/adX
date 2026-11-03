// golden_all_formats: the phase gate (phase_8.md §3.9, §6). Every fixture through every
// export format.
//
// The lossless formats are hashed after decoding - WAV at 16, 24 and 32-bit float,
// FLAC at 16 and 24 - into tests/golden/formats.txt, so a change to an encoder, to the
// dither, or to anything that renders is caught by name. The dither is deterministic
// (Dither.h), which is what lets a 16-bit file have a hash at all.
//
// MP3 and OGG are not byte-hashed: an encoder's output is not promised to be stable
// across its own builds, and a test that pretended otherwise would fail for the wrong
// reason and get switched off. They are decoded and compared with the float render
// band by band - third-octaves to 16 kHz, OGG within 1 dB down to 40 dB under the
// loudest band, MP3 within 2 dB down to 30 dB - which is what "it still sounds like the
// song" means for a lossy codec.
//
// [.slow]: 9 formats of 10 fixtures, ten seconds each. Per push in the optimised builds.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <map>
#include <numbers>
#include <string>
#include <vector>

#include "engine/dsp/Fft.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/audio/AudioFileLoader.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/render/ExportJob.h"
#include "engine/render/RenderHash.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/clip/ClipFixtures.h"
#include "tests/cpp/render/GoldenHashes.h"

namespace fs = std::filesystem;

namespace {

struct FormatCase {
    const char* name{};
    adx::render::FileFormat format{};
    std::uint32_t flacBits{24};
    bool lossy{false};
};

constexpr std::array<FormatCase, 7> kFormats{{
    {.name = "wav16", .format = adx::render::FileFormat::Wav16},
    {.name = "wav24", .format = adx::render::FileFormat::Wav24},
    {.name = "wav32f", .format = adx::render::FileFormat::Wav32f},
    {.name = "flac16", .format = adx::render::FileFormat::Flac, .flacBits = 16},
    {.name = "flac24", .format = adx::render::FileFormat::Flac, .flacBits = 24},
    {.name = "mp3", .format = adx::render::FileFormat::Mp3, .lossy = true},
    {.name = "ogg", .format = adx::render::FileFormat::Ogg, .lossy = true},
}};

std::vector<float> decodeInterleaved(const fs::path& path) {
    adx::format::SampleBuffer buffer;
    std::string error;
    REQUIRE(adx::format::decodeFile(path, buffer, error));
    std::vector<float> out;
    out.reserve(buffer.frames * 2);
    for (std::size_t i = 0; i < buffer.frames; ++i) {
        out.push_back(buffer.leftAudio()[i]);
        out.push_back(buffer.rightAudio()[i]);
    }
    return out;
}

/// 31 third-octave band levels in dB, 20 Hz to 20 kHz, of the mono sum, from Hann-windowed
/// 8192-point frames at half overlap.
std::vector<double> thirdOctaves(const std::vector<float>& stereo, std::uint32_t rate) {
    constexpr std::size_t kSize = 8192;
    adx::dsp::Fft fft;
    fft.prepare(kSize);
    std::vector<adx::dsp::Complex> work(kSize);
    std::vector<double> power((kSize / 2) + 1, 0.0);
    const std::size_t frames = stereo.size() / 2;
    for (std::size_t start = 0; start + kSize < frames; start += kSize / 2) {
        for (std::size_t n = 0; n < kSize; ++n) {
            const double w = 0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) /
                                                   static_cast<double>(kSize - 1)));
            const double mono = 0.5 * (stereo[(start + n) * 2] + stereo[((start + n) * 2) + 1]);
            work[n] = adx::dsp::Complex{static_cast<float>(mono * w), 0.0F};
        }
        fft.forward(work);
        for (std::size_t k = 0; k < power.size(); ++k) {
            power[k] += std::norm(work[k]);
        }
    }
    std::vector<double> levels;
    const double binHz = static_cast<double>(rate) / kSize;
    for (int band = 0; band < 31; ++band) {
        const double centre = 1000.0 * std::pow(2.0, (band - 17) / 3.0);
        const double low = centre * std::pow(2.0, -1.0 / 6.0);
        const double high = centre * std::pow(2.0, 1.0 / 6.0);
        double sum = 0.0;
        for (std::size_t k = 1; k < power.size(); ++k) {
            const double f = static_cast<double>(k) * binHz;
            if (f >= low && f < high) {
                sum += power[k];
            }
        }
        levels.push_back(10.0 * std::log10(sum + 1e-30));
    }
    return levels;
}

/// Fixtures whose MP3 is known to be wrong, and why. STAKILLAZ's blown master is
/// full-scale, hard-clipped bass (a 2 dB crest factor): shine's subband filter sums 64
/// fixed-point products in 32 bits and overflows on it, so the decoded MP3 peaks at 2.3
/// with +20 dB below 50 Hz, where LAME on the same samples is within 0.1 dB (STATE.md
/// P8-5). Checked the other way round below, so a fix shows up as a failure to remove
/// the entry.
bool knownBadMp3(const std::string& name) {
    return name == "tests/golden/genre_stakillaz_seed1.adx";
}

} // namespace

TEST_CASE("golden_all_formats", "[render][golden][export][.slow]") {
    adx::tests::ClipDir const dir;
    std::vector<fs::path> fixtures;
    for (const auto& entry : fs::directory_iterator(adx::tests::goldenDirectory())) {
        if (entry.path().extension() == ".adx") {
            fixtures.push_back(entry.path());
        }
    }
    for (const auto& example : adx::tests::corpusPaths()) {
        fixtures.push_back(example);
    }
    std::ranges::sort(fixtures);

    std::map<std::string, std::string> actual;
    for (const fs::path& path : fixtures) {
        adx::project::Project project;
        adx::project::CommandStack stack;
        adx::format::DiagnosticList diagnostics;
        adx::format::load(adx::tests::readFile(path), project, stack, diagnostics,
                          adx::format::ParseOptions{.baseDirectory = path.parent_path().string()});
        const std::string name = fs::relative(path, adx::tests::repoRoot()).generic_string();
        std::vector<float> reference;
        for (const FormatCase& format : kFormats) {
            INFO(name << " " << format.name);
            adx::render::ExportRequest request;
            request.output = dir.path() / (path.stem().string() + "." + format.name +
                                           adx::render::extensionOf(format.format));
            request.encode.format = format.format;
            request.encode.flacBits = format.flacBits;
            // The first ten seconds, as the corpus has always been rendered.
            request.range = adx::render::RangeKind::Selection;
            request.to = std::min(
                project.contentLength(),
                project.tempo.toTicks(adx::core::Samples{std::int64_t{48000} * 10}, 48000));
            request.tailSeconds = 0.25;
            const adx::render::ExportResult result = adx::render::runExport(project, request);
            INFO(result.error);
            REQUIRE(result.ok);
            const std::vector<float> decoded = decodeInterleaved(result.files.front().path);
            if (format.format == adx::render::FileFormat::Wav32f) {
                reference = decoded;
            }
            if (!format.lossy) {
                actual[name + ":" + format.name] = adx::render::hashSamples(decoded).hex();
                continue;
            }
            REQUIRE_FALSE(reference.empty());
            const std::vector<double> want = thirdOctaves(reference, 48000);
            const std::vector<double> got = thirdOctaves(decoded, 48000);
            const double loudest = *std::ranges::max_element(want);
            if (format.format == adx::render::FileFormat::Mp3 && knownBadMp3(name)) {
                // Still broken: the overflow's bass is there. Fixed, this fails.
                CHECK(got[3] - want[3] > 10.0);
                continue;
            }
            // Measured over this corpus (phase_8.md §11): Vorbis at quality 0.6 within
            // 0.56 dB down to 40 dB below the loudest band; shine at 320 kbps within
            // 0.94 dB down to 20 dB, 1.97 down to 30, 2.46 down to 40. Below that each
            // codec's noise is the band.
            const double window = format.format == adx::render::FileFormat::Ogg ? 40.0 : 30.0;
            const double tolerance = format.format == adx::render::FileFormat::Ogg ? 1.0 : 2.0;
            // Up to the 16 kHz band: shine low-passes above it.
            for (std::size_t band = 0; band < 30; ++band) {
                if (want[band] < loudest - window) {
                    continue;
                }
                INFO("band " << band);
                CHECK(std::abs(got[band] - want[band]) <= tolerance);
            }
        }
    }
    adx::tests::checkGolden(adx::tests::goldenDirectory() / "formats.txt",
                            "# Decoded hashes of every export format of every golden fixture\n"
                            "# (tests/cpp/render/test_golden_formats.cpp): 48 kHz, 256-frame\n"
                            "# blocks, the first ten seconds, TPDF dither below 32-bit. MP3 and\n"
                            "# OGG are compared spectrally instead. Regenerate with\n"
                            "# ADX_UPDATE_GOLDEN=1, and only on purpose.\n",
                            actual);
}
