// The export matrix (phase_8.md §3.5, §5): every format decodes back, dither only when
// the depth drops, loudness normalization, tails, ranges, splits - and stems that sum
// to the master.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/dsp/Loudness.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/audio/AudioFileLoader.h"
#include "engine/render/ExportJob.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/clip/ClipFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace fs = std::filesystem;
using adx::render::ExportRequest;
using adx::render::FileFormat;
using adx::tests::ClipDir;
using adx::tests::loadText;

namespace {

constexpr std::uint32_t kRate = 48000;

/// Two bars of a held chord into a reverb, and a marker per bar: enough sound to
/// encode, a tail that rings, markers to split at, and a loop to export.
constexpr const char* kSong = R"([PROJECT]
ADX_VERSION=2
LOOP=0:2:0-1:2:0

[TEMPO]
0:0:0 120

[CHANNEL Keys]
INSTRUMENT=additive
OUTPUT=insert.2

[PATTERN Chord]
LENGTH=1:0:0
NOTES Keys
  C4 0:0:0 0:2:0 100
  E4 0:0:0 0:2:0 100
  G4 0:2:0 0:2:0 90

[PLAYLIST]
TRACK 1
  PATTERN Chord 0:0:0
  PATTERN Chord 1:0:0

[MIXER]
INSERT 1 name="Master"
INSERT 2 name="Keys"
  SLOT 1 Reverb mix=0.5 room=0.9
ROUTE insert.2 -> insert.1

[MARKERS]
1:0:0 Second
)";

adx::format::SampleBuffer decode(const fs::path& path) {
    adx::format::SampleBuffer buffer;
    std::string error;
    REQUIRE(adx::format::decodeFile(path, buffer, error));
    return buffer;
}

std::vector<float> interleaved(const adx::format::SampleBuffer& buffer) {
    std::vector<float> out;
    const auto left = buffer.leftAudio();
    const auto right = buffer.rightAudio();
    for (std::size_t i = 0; i < left.size(); ++i) {
        out.push_back(left[i]);
        out.push_back(right[i]);
    }
    return out;
}

adx::render::ExportResult exportWith(const adx::project::Project& project,
                                     const ExportRequest& request) {
    adx::render::ExportResult result = adx::render::runExport(project, request);
    INFO(result.error);
    REQUIRE(result.ok);
    return result;
}

} // namespace

TEST_CASE("export_all_formats", "[render][export]") {
    ClipDir const dir;
    const auto loaded = loadText(kSong);
    struct Case {
        FileFormat format;
        std::uint32_t flacBits;
        std::uint32_t rate;
    };
    for (const Case c : {Case{.format = FileFormat::Wav16, .flacBits = 24, .rate = 48000},
                         Case{.format = FileFormat::Wav24, .flacBits = 24, .rate = 48000},
                         Case{.format = FileFormat::Wav32f, .flacBits = 24, .rate = 48000},
                         Case{.format = FileFormat::Flac, .flacBits = 16, .rate = 48000},
                         Case{.format = FileFormat::Flac, .flacBits = 24, .rate = 44100},
                         Case{.format = FileFormat::Mp3, .flacBits = 24, .rate = 44100},
                         Case{.format = FileFormat::Mp3, .flacBits = 24, .rate = 48000},
                         Case{.format = FileFormat::Ogg, .flacBits = 24, .rate = 48000}}) {
        INFO(adx::render::toString(c.format) << " " << c.flacBits << " " << c.rate);
        ExportRequest request;
        request.output = dir.path() / (std::string("song_") + adx::render::toString(c.format) +
                                       std::to_string(c.rate) + adx::render::extensionOf(c.format));
        request.encode.format = c.format;
        request.encode.flacBits = c.flacBits;
        request.encode.sampleRate = c.rate;
        const adx::render::ExportResult result = exportWith(loaded->project, request);
        REQUIRE(result.files.size() == 1);
        const adx::format::SampleBuffer back = decode(result.files[0].path);
        CHECK(back.sampleRate == c.rate);
        CHECK(back.sourceChannels == 2);
        const auto expected = static_cast<std::int64_t>(result.files[0].frames);
        const auto got = static_cast<std::int64_t>(back.frames);
        if (c.format == FileFormat::Mp3) {
            // No LAME tag: the decoder knows neither the encoder's delay nor its padding,
            // so the length is right to within a frame or two of granules.
            CHECK(std::abs(got - expected) <= (2 * 1152) + 1105);
        } else {
            CHECK(got == expected);
        }
    }
}

TEST_CASE("export_dither_only_on_reduction", "[render][export]") {
    ClipDir const dir;
    const auto loaded = loadText(kSong);
    ExportRequest request;
    request.output = dir.path() / "float.wav";
    request.encode.format = FileFormat::Wav32f;
    request.dither = adx::render::DitherKind::Shaped;
    const adx::render::ExportResult result = exportWith(loaded->project, request);

    adx::render::OfflineRenderOptions const options;
    adx::render::RenderStats stats;
    const std::vector<float> render = adx::render::renderOffline(loaded->project, options, stats);
    // Bit for bit the render: dither was asked for and correctly not applied.
    REQUIRE(interleaved(decode(result.files[0].path)) == render);

    // At 16 bits it is applied: every sample is a multiple of one LSB, and the file is
    // not simply the render rounded.
    request.output = dir.path() / "short.wav";
    request.encode.format = FileFormat::Wav16;
    const std::vector<float> shortened =
        interleaved(decode(exportWith(loaded->project, request).files[0].path));
    std::size_t differsFromRounding = 0;
    for (std::size_t i = 0; i < render.size(); ++i) {
        REQUIRE(shortened[i] * 32768.0F == std::nearbyint(shortened[i] * 32768.0F));
        if (shortened[i] != std::nearbyint(render[i] * 32768.0F) / 32768.0F) {
            ++differsFromRounding;
        }
    }
    CHECK(differsFromRounding > render.size() / 4);
}

TEST_CASE("export_normalize_lufs", "[render][export]") {
    ClipDir const dir;
    const auto loaded = loadText(kSong);
    ExportRequest request;
    request.output = dir.path() / "loud.wav";
    request.encode.format = FileFormat::Wav32f;
    request.normalize = adx::render::NormalizeKind::Lufs;
    request.normalizeTarget = -14.0F;
    const adx::render::ExportResult result = exportWith(loaded->project, request);
    const adx::format::SampleBuffer back = decode(result.files[0].path);
    adx::dsp::LoudnessMeter meter;
    meter.prepare(back.sampleRate);
    meter.process(back.leftAudio(), back.rightAudio());
    CHECK_THAT(meter.integrated(), Catch::Matchers::WithinAbs(-14.0, 0.1));
    CHECK(result.gainDb != 0.0);

    request.output = dir.path() / "peak.wav";
    request.normalize = adx::render::NormalizeKind::Peak;
    request.normalizeTarget = -1.0F;
    const adx::render::ExportResult peaked = exportWith(loaded->project, request);
    CHECK_THAT(20.0 * std::log10(peaked.files[0].stats.peak),
               Catch::Matchers::WithinAbs(-1.0, 1e-4));
}

TEST_CASE("export_tail_until_silence", "[render][export]") {
    ClipDir const dir;
    const auto loaded = loadText(kSong);
    ExportRequest request;
    request.output = dir.path() / "tail.wav";
    request.encode.format = FileFormat::Wav32f;
    request.tailUntilSilence = true;
    request.silenceDb = -80.0F;
    const adx::render::ExportResult result = exportWith(loaded->project, request);
    const std::uint64_t range = 4 * static_cast<std::size_t>(kRate); // two bars at 120
    const std::uint64_t frames = result.files[0].frames;
    // A room of 0.9 rings for seconds; the file holds it.
    CHECK(frames > range + kRate);

    // Rendered on for longer, nothing after the file's end is above the threshold: it
    // did not stop mid-decay.
    adx::render::OfflineRenderOptions options;
    options.tailSeconds = static_cast<double>(frames - range) / kRate + 2.0;
    adx::render::RenderStats stats;
    const std::vector<float> longer = adx::render::renderOffline(loaded->project, options, stats);
    const auto threshold = static_cast<float>(std::pow(10.0, -80.0 / 20.0));
    for (std::size_t i = frames * 2; i < longer.size(); ++i) {
        REQUIRE(std::abs(longer[i]) <= threshold);
    }
    REQUIRE(std::abs(longer[(frames - 1) * 2]) + std::abs(longer[(frames * 2) - 1]) > 0.0F);
}

TEST_CASE("export_split_at_markers", "[render][export]") {
    ClipDir const dir;
    const auto loaded = loadText(kSong);
    ExportRequest request;
    request.output = dir.path() / "whole.wav";
    request.encode.format = FileFormat::Wav32f;
    const std::vector<float> whole =
        interleaved(decode(exportWith(loaded->project, request).files[0].path));

    request.output = dir.path() / "split.wav";
    request.splitAtMarkers = true;
    const adx::render::ExportResult split = exportWith(loaded->project, request);
    // One marker: two files, cut exactly at it, nothing dropped.
    REQUIRE(split.files.size() == 2);
    CHECK(split.files[0].frames == 2 * static_cast<std::size_t>(kRate));
    std::vector<float> joined;
    for (const adx::render::ExportedFile& file : split.files) {
        const std::vector<float> piece = interleaved(decode(file.path));
        joined.insert(joined.end(), piece.begin(), piece.end());
    }
    REQUIRE(joined == whole);
}

TEST_CASE("export_range_loop_only", "[render][export]") {
    ClipDir const dir;
    const auto loaded = loadText(kSong);
    ExportRequest request;
    request.output = dir.path() / "loop.wav";
    request.encode.format = FileFormat::Wav32f;
    request.range = adx::render::RangeKind::Loop;
    const adx::render::ExportResult result = exportWith(loaded->project, request);
    // LOOP=0:2:0-1:2:0 is one bar: two seconds at 120 bpm, to the sample.
    CHECK(result.files[0].frames == 2 * static_cast<std::size_t>(kRate));

    // And it is the same samples a render of that range gives.
    adx::render::OfflineRenderOptions options;
    options.start = adx::core::Ticks{adx::core::kPpq * 2};
    options.end = adx::core::Ticks{adx::core::kPpq * 6};
    adx::render::RenderStats stats;
    REQUIRE(interleaved(decode(result.files[0].path)) ==
            adx::render::renderOffline(loaded->project, options, stats));
}

TEST_CASE("export_job_runs_off_thread_and_cancels", "[render][export]") {
    ClipDir const dir;
    const auto loaded = loadText(kSong);
    ExportRequest request;
    request.output = dir.path() / "job.wav";
    request.encode.format = FileFormat::Wav24;
    {
        adx::render::ExportJob job(loaded->project, request);
        const adx::render::ExportResult& result = job.wait();
        CHECK(result.ok);
        CHECK(job.progress() == 1.0F);
    }
    request.tailSeconds = 600.0; // long enough to still be running when cancelled
    adx::render::ExportJob job(loaded->project, request);
    job.cancel();
    const adx::render::ExportResult& result = job.wait();
    CHECK_FALSE(result.ok);
    CHECK(result.cancelled);
}

// Stems sum to the master wherever the processing that the stems share is linear in the
// signal - sends, buses, delay, reverb, EQ - or is dynamics, whose gain each stem's copy
// takes from the full mix (SlotNode::canFollowKey): a compressor, a limiter, a gate, a
// sidechained ducker. A waveshaper on a bus two stems reach cannot be split by anybody:
// drive(a) + drive(b) is not drive(a + b). So those slots - and only those - are bypassed
// first, and the test says which. Two of the corpus have one on their master (v1's
// MASTER_DRIVE): suffocation.adx and stakillaz_demo.adx.
//
// [.slow]: every stem of eight bars of every fixture, in three modes, is about seven
// minutes in Debug. Per push it runs in the optimised builds (plans/STATE.md).
TEST_CASE("stems_sum_to_master", "[render][export][stems][.slow]") {
    ClipDir const dir;
    std::vector<fs::path> fixtures = adx::tests::corpusPaths();
    for (const fs::path& golden : fs::directory_iterator(adx::tests::repoRoot() / "tests/golden")) {
        if (golden.extension() == ".adx") {
            fixtures.push_back(golden);
        }
    }
    for (const fs::path& path : fixtures) {
        adx::project::Project project;
        adx::project::CommandStack stack;
        adx::format::DiagnosticList diagnostics;
        adx::format::load(adx::tests::readFile(path), project, stack, diagnostics,
                          adx::format::ParseOptions{.baseDirectory = path.parent_path().string()});
        INFO(path.filename().string());
        REQUIRE_FALSE(diagnostics.hasErrors());
        std::size_t shapers = 0;
        for (adx::project::Insert& insert : project.mixer.inserts) {
            for (adx::project::Slot& slot : insert.slots) {
                for (const char* type :
                     {"Distortion", "Overdrive", "Saturation", "Bitcrush", "MultibandComp",
                      "TransientShaper", "PitchShifter", "SpectralFreeze"}) {
                    if (slot.type == type && !slot.bypass) {
                        slot.bypass = true;
                        ++shapers;
                    }
                }
            }
        }
        for (const adx::render::StemMode mode :
             {adx::render::StemMode::Channels, adx::render::StemMode::Inserts,
              adx::render::StemMode::Patterns}) {
            INFO(path.filename().string() << " " << adx::render::toString(mode) << ", " << shapers
                                          << " waveshaping slot(s) bypassed");
            ExportRequest request;
            request.output = dir.path() / (path.stem().string() + ".wav");
            request.encode.format = FileFormat::Wav32f;
            request.stems = mode;
            // Eight bars of each: every send, bus and sidechain in the corpus is busy by
            // then, and the whole of each song would take most of an hour in Debug.
            request.range = adx::render::RangeKind::Selection;
            request.from = adx::core::Ticks{0};
            request.to = adx::core::Ticks{adx::core::kPpq * 4 * 8};
            request.tailSeconds = 1.0;
            const adx::render::ExportResult result = exportWith(project, request);
            REQUIRE(result.files.size() >= 2);
            const std::vector<float> master = interleaved(decode(result.files[0].path));
            std::vector<double> sum(master.size(), 0.0);
            for (std::size_t f = 1; f < result.files.size(); ++f) {
                const std::vector<float> stem = interleaved(decode(result.files[f].path));
                REQUIRE(stem.size() == master.size());
                for (std::size_t i = 0; i < stem.size(); ++i) {
                    sum[i] += stem[i];
                }
            }
            double worst = 0.0;
            for (std::size_t i = 0; i < master.size(); ++i) {
                worst = std::max(worst, std::abs(sum[i] - master[i]));
            }
            CHECK(worst <= 1e-6);
        }
    }
}
