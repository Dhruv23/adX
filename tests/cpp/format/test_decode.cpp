// Audio decode and the sample pool (phase_4.md §4.11, §6).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <span>
#include <string>
#include <vector>

#include "engine/format/audio/AudioFileLoader.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/project/Resources.h"
#include "tests/cpp/Corpus.h"

namespace fs = std::filesystem;
using adx::format::SampleBuffer;

namespace {

constexpr std::uint32_t kRate = 44100;
constexpr std::uint32_t kFrames = kRate / 4; // 0.25 s, as tools/gen_audio_fixtures.py makes

/// The fixture signal: 440 Hz left, 660 Hz right, amplitude 0.5.
double fixture(std::uint32_t channel, std::uint32_t frame) {
    const double frequency = channel == 0 ? 440.0 : 660.0;
    return 0.5 * std::sin(2.0 * std::numbers::pi * frequency * frame / kRate);
}

void put16le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8U));
}
void put32le(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16le(out, v & 0xFFFFU);
    put16le(out, v >> 16U);
}
void put16be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8U));
    out.push_back(static_cast<std::uint8_t>(v));
}
void put32be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16be(out, v >> 16U);
    put16be(out, v & 0xFFFFU);
}
void tag(std::vector<std::uint8_t>& out, const char* fourcc) {
    out.insert(out.end(), fourcc, fourcc + 4);
}

/// A WAV of the fixture signal: format 1 (PCM, 8/16/24 bit) or 3 (float, 32 bit).
std::vector<std::uint8_t> makeWav(std::uint16_t bits, bool floating) {
    const std::uint32_t bytesPerSample = bits / 8U;
    const std::uint32_t dataBytes = kFrames * 2U * bytesPerSample;
    std::vector<std::uint8_t> out;
    tag(out, "RIFF");
    put32le(out, 36U + dataBytes);
    tag(out, "WAVE");
    tag(out, "fmt ");
    put32le(out, 16);
    put16le(out, floating ? 3U : 1U);
    put16le(out, 2);
    put32le(out, kRate);
    put32le(out, kRate * 2U * bytesPerSample);
    put16le(out, 2U * bytesPerSample);
    put16le(out, bits);
    tag(out, "data");
    put32le(out, dataBytes);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < 2; ++c) {
            const double x = fixture(c, f);
            if (floating) {
                const auto value = static_cast<float>(x);
                std::uint32_t raw = 0;
                std::memcpy(&raw, &value, 4);
                put32le(out, raw);
            } else if (bits == 8) {
                out.push_back(static_cast<std::uint8_t>(std::lround((x * 127.0) + 128.0)));
            } else {
                const auto full =
                    static_cast<std::int32_t>(std::lround(x * ((1 << (bits - 1)) - 1)));
                const auto raw = static_cast<std::uint32_t>(full);
                for (std::uint32_t b = 0; b < bytesPerSample; ++b) {
                    out.push_back(static_cast<std::uint8_t>(raw >> (8U * b)));
                }
            }
        }
    }
    return out;
}

/// An AIFF (big-endian, 16-bit) of the fixture signal.
std::vector<std::uint8_t> makeAiff() {
    const std::uint32_t dataBytes = kFrames * 2U * 2U;
    std::vector<std::uint8_t> out;
    tag(out, "FORM");
    put32be(out, 4U + (8U + 18U) + (8U + 8U + dataBytes));
    tag(out, "AIFF");
    tag(out, "COMM");
    put32be(out, 18);
    put16be(out, 2);
    put32be(out, kFrames);
    put16be(out, 16);
    // 44100 as an IEEE 754 80-bit extended float.
    constexpr std::array<std::uint8_t, 10> kExtended44100{0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
    out.insert(out.end(), kExtended44100.begin(), kExtended44100.end());
    tag(out, "SSND");
    put32be(out, 8U + dataBytes);
    put32be(out, 0);
    put32be(out, 0);
    for (std::uint32_t f = 0; f < kFrames; ++f) {
        for (std::uint32_t c = 0; c < 2; ++c) {
            const auto value = static_cast<std::int32_t>(std::lround(fixture(c, f) * 32767.0));
            put16be(out, static_cast<std::uint32_t>(value) & 0xFFFFU);
        }
    }
    return out;
}

fs::path scratchDirectory() {
    const fs::path dir = fs::temp_directory_path() / "adx_decode_tests";
    fs::create_directories(dir);
    return dir;
}

fs::path writeFile(const std::string& name, const std::vector<std::uint8_t>& bytes) {
    const fs::path path = scratchDirectory() / name;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return path;
}

double rms(std::span<const float> x) {
    double sum = 0.0;
    for (const float s : x) {
        sum += static_cast<double>(s) * s;
    }
    return std::sqrt(sum / static_cast<double>(x.size()));
}

std::size_t risingCrossings(std::span<const float> x) {
    std::size_t n = 0;
    for (std::size_t i = 1; i < x.size(); ++i) {
        n += (x[i - 1] < 0.0F && x[i] >= 0.0F) ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("decode_all_formats", "[format][decode]") {
    struct Case {
        fs::path path;
        bool lossy;
    };
    const fs::path data = adx::tests::repoRoot() / "tests" / "data" / "audio";
    const std::vector<Case> cases{
        {.path = writeFile("tone_u8.wav", makeWav(8, false)), .lossy = false},
        {.path = writeFile("tone_s16.wav", makeWav(16, false)), .lossy = false},
        {.path = writeFile("tone_s24.wav", makeWav(24, false)), .lossy = false},
        {.path = writeFile("tone_f32.wav", makeWav(32, true)), .lossy = false},
        {.path = writeFile("tone.aiff", makeAiff()), .lossy = false},
        {.path = data / "tone.flac", .lossy = false},
        {.path = data / "tone.ogg", .lossy = true},
        {.path = data / "tone.mp3", .lossy = true},
    };
    const double expectedRms = 0.5 / std::numbers::sqrt2;
    for (const Case& c : cases) {
        INFO(c.path.string());
        SampleBuffer buffer;
        std::string error;
        REQUIRE(adx::format::decodeFile(c.path, buffer, error));
        CHECK(error.empty());
        CHECK(buffer.sampleRate == kRate);
        CHECK(buffer.sourceChannels == 2);
        REQUIRE(buffer.rightAudio().size() == buffer.leftAudio().size());
        if (c.lossy) {
            // An encoder pads: MP3's frames are 1152 samples and its decoder delay is
            // ~1100 more. The length is within that, never short.
            CHECK(buffer.frames >= kFrames - 1);
            CHECK(buffer.frames <= kFrames + 2304 + 1152);
        } else {
            CHECK(buffer.frames == kFrames);
        }
        // Level within 0.5 dB (8-bit quantisation and a lossy codec both fit), and the
        // channels in order: the left carries 440 Hz, the right 660.
        // A lossy file's padding is silence, so its RMS over the padded length is
        // lower by the square root of the length ratio.
        const double padding = std::sqrt(static_cast<double>(buffer.frames) / kFrames);
        CHECK(std::abs(20.0 * std::log10(rms(buffer.leftAudio()) * padding / expectedRms)) < 0.5);
        CHECK(std::abs(20.0 * std::log10(rms(buffer.rightAudio()) * padding / expectedRms)) < 0.5);
        const double seconds = static_cast<double>(kFrames) / kRate;
        CHECK_THAT(static_cast<double>(risingCrossings(buffer.leftAudio())),
                   Catch::Matchers::WithinAbs(440.0 * seconds, 3.0));
        CHECK_THAT(static_cast<double>(risingCrossings(buffer.rightAudio())),
                   Catch::Matchers::WithinAbs(660.0 * seconds, 3.0));
    }
}

TEST_CASE("decode_reports_failure", "[format][decode]") {
    SampleBuffer buffer;
    std::string error;
    CHECK_FALSE(adx::format::decodeFile(scratchDirectory() / "does_not_exist.wav", buffer, error));
    CHECK_FALSE(error.empty());
    error.clear();
    const fs::path garbage =
        writeFile("garbage.wav", {'n', 'o', 't', ' ', 'a', 'u', 'd', 'i', 'o'});
    CHECK_FALSE(adx::format::decodeFile(garbage, buffer, error));
    CHECK_FALSE(error.empty());

    adx::format::SamplePool pool(1);
    const auto entry = pool.request(garbage);
    pool.waitAll();
    CHECK(entry->handle().state() == adx::format::SampleState::Failed);
    CHECK_FALSE(entry->error().empty());
    CHECK(entry->buffer() == nullptr);
}

TEST_CASE("decode_dedup", "[format][decode]") {
    // phase_4.md §6: two clips on one file share one buffer, and its refcount reaches 0
    // exactly once. And the same content under another name decodes once.
    const fs::path original = writeFile("dedup_a.wav", makeWav(16, false));
    const fs::path copy = writeFile("dedup_b.wav", makeWav(16, false));
    adx::format::SamplePool pool(2);
    {
        const auto first = pool.request(original);
        const auto second = pool.request(original);
        CHECK(first == second); // one entry per path
        pool.waitAll();
        REQUIRE(first->handle().ready());
        const auto other = pool.request(copy);
        pool.waitAll();
        REQUIRE(other->handle().ready());
        CHECK(other != first);
        CHECK(other->buffer() == first->buffer()); // one buffer per content
        CHECK(other->handle().view().left == first->handle().view().left);

        const adx::format::SamplePoolStats during = pool.stats();
        CHECK(during.requests == 3);
        CHECK(during.decodes == 1);
        CHECK(during.liveEntries == 2);
        CHECK(during.buffersFreed == 0);
    }
    // Every holder gone: the buffer was freed, once.
    const adx::format::SamplePoolStats after = pool.stats();
    CHECK(after.liveEntries == 0);
    CHECK(after.buffersFreed == 1);

    // And a later request decodes it afresh rather than finding a dangling entry.
    const auto again = pool.request(original);
    pool.waitAll();
    CHECK(again->handle().ready());
    CHECK(pool.stats().decodes == 2);
}

TEST_CASE("sample_pool_resolves_paths", "[format][decode]") {
    const fs::path library = scratchDirectory() / "library";
    fs::create_directories(library / "drums");
    {
        std::ofstream(library / "drums" / "kick.wav", std::ios::binary) << "x";
        std::ofstream(library / "snare.wav", std::ios::binary) << "x";
    }
    adx::project::Resources resources;
    resources.baseDirectory = (scratchDirectory() / "project").string();
    fs::create_directories(resources.baseDirectory);
    adx::format::SamplePool pool(1);

    // Not beside the project, no search paths: the project-relative place, which is
    // where the "missing" diagnostic should point.
    CHECK(pool.resolve(resources, "drums/kick.wav") ==
          fs::path(resources.baseDirectory) / "drums/kick.wav");
    pool.addSearchPath(library);
    // By relative path under a search path.
    CHECK(pool.resolve(resources, "drums/kick.wav") == library / "drums/kick.wav");
    // And by file name when the folder structure differs.
    CHECK(pool.resolve(resources, "old/folder/snare.wav") == library / "snare.wav");
}
