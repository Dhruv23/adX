#include "tests/cpp/instruments/VoicebankFixture.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace adx::tests {
namespace {

namespace fs = std::filesystem;

constexpr int kRate = 44100;

/// A path from UTF-8 text (see Voicebank.cpp: the narrow constructor is ANSI on Windows).
fs::path u8(std::string_view text) {
    std::u8string s;
    for (const char c : text) {
        s.push_back(static_cast<char8_t>(c));
    }
    return fs::path{s};
}

void writePcm16(const fs::path& path, const std::vector<float>& samples) {
    std::ofstream out(path, std::ios::binary);
    const auto put32 = [&out](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    const auto put16 = [&out](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
    out.write("RIFF", 4);
    put32(36 + bytes);
    out.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(1);
    put32(kRate);
    put32(kRate * 2);
    put16(2);
    put16(16);
    out.write("data", 4);
    put32(bytes);
    for (const float s : samples) {
        const auto v =
            static_cast<std::int16_t>(std::lround(std::clamp(s, -1.0F, 1.0F) * 32767.0F));
        out.write(reinterpret_cast<const char*>(&v), 2);
    }
}

void writeFrq(const fs::path& path, std::size_t samples) {
    std::ofstream out(path, std::ios::binary);
    out.write("FREQ0003", 8);
    const std::int32_t hop = 256;
    out.write(reinterpret_cast<const char*>(&hop), 4);
    const double average = kSyntheticF0;
    out.write(reinterpret_cast<const char*>(&average), 8);
    const std::array<char, 16> blank{};
    out.write(blank.data(), 16);
    const auto count = static_cast<std::int32_t>(samples / 256);
    out.write(reinterpret_cast<const char*>(&count), 4);
    for (std::int32_t k = 0; k < count; ++k) {
        const double f0 = kSyntheticF0;
        const double amplitude = 1000.0;
        out.write(reinterpret_cast<const char*>(&f0), 8);
        out.write(reinterpret_cast<const char*>(&amplitude), 8);
    }
}

struct Formants {
    std::array<double, 3> hz;
};

// a i u e o
constexpr std::array<Formants, 5> kVowels{{
    {{730.0, 1090.0, 2440.0}},
    {{270.0, 2290.0, 3010.0}},
    {{300.0, 870.0, 2240.0}},
    {{530.0, 1840.0, 2480.0}},
    {{570.0, 840.0, 2410.0}},
}};

/// `seconds` of a vowel sliding from `from` to `to` formants halfway through, preceded
/// by `noise` seconds of a noise burst.
std::vector<float> sing(const std::vector<int>& vowels, double secondsPerVowel, double noise) {
    std::vector<float> out;
    std::uint32_t seed = 12345U;
    const auto random = [&seed] {
        seed ^= seed << 13U;
        seed ^= seed >> 17U;
        seed ^= seed << 5U;
        return ((static_cast<double>(seed) / 4294967295.0) * 2.0) - 1.0;
    };
    const auto noiseFrames = static_cast<std::size_t>(noise * kRate);
    out.reserve(noiseFrames + (static_cast<std::size_t>(secondsPerVowel * kRate) * vowels.size()));
    for (std::size_t n = 0; n < noiseFrames; ++n) {
        out.push_back(static_cast<float>(0.3 * random()));
    }
    // Two-pole resonators per formant, driven by a band-limited pulse train.
    std::array<std::array<double, 2>, 3> state{};
    double phase = 0.0;
    const auto perVowel = static_cast<std::size_t>(secondsPerVowel * kRate);
    for (const int vowel : vowels) {
        for (std::size_t n = 0; n < perVowel; ++n) {
            const Formants& now = kVowels[static_cast<std::size_t>(vowel)];
            phase += kSyntheticF0 / kRate;
            phase -= std::floor(phase);
            // A sum of harmonics up to 5 kHz: the source, band-limited.
            double source = 0.0;
            for (int h = 1; h * kSyntheticF0 < 5000.0; ++h) {
                source += std::sin(2.0 * std::numbers::pi * h * phase) / h;
            }
            double y = 0.0;
            for (std::size_t f = 0; f < 3; ++f) {
                const double r = std::exp(-std::numbers::pi * 80.0 / kRate);
                const double c = 2.0 * r * std::cos(2.0 * std::numbers::pi * now.hz[f] / kRate);
                const double s = (source * (1.0 - r)) + (c * state[f][0]) - (r * r * state[f][1]);
                state[f][1] = state[f][0];
                state[f][0] = s;
                y += s / static_cast<double>(f + 1);
            }
            out.push_back(static_cast<float>(0.25 * y));
        }
    }
    // 30 ms fades.
    const std::size_t fade = kRate * 3 / 100;
    for (std::size_t n = 0; n < fade && n < out.size(); ++n) {
        const auto g = static_cast<float>(n) / static_cast<float>(fade);
        out[n] *= g;
        out[out.size() - 1 - n] *= g;
    }
    return out;
}

void write(const fs::path& dir, std::string_view name, const std::vector<float>& samples) {
    const fs::path wav = dir / u8(std::string(name) + ".wav");
    writePcm16(wav, samples);
    writeFrq(dir / u8(std::string(name) + "_wav.frq"), samples.size());
}

void writeBytes(const fs::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

fs::path build() {
    const fs::path root = fs::temp_directory_path() / "adx_voicebank_synth";
    fs::remove_all(root);
    // Shift-JIS spellings: 単独音 連続音 for the folders' names in the oto files is not
    // needed - the folders are named in UTF-8 by the OS - but every oto.ini and
    // character.txt below is Shift-JIS bytes, as Teto's are.
    const fs::path cv = root / u8("単独音");
    const fs::path vcv = root / u8("連続音");
    fs::create_directories(cv);
    fs::create_directories(vcv);
    // character.txt: "name=合成テスト"
    writeBytes(root / "character.txt", "name=\x8D\x87\x90\xAC\x83\x65\x83\x58\x83\x67\r\n");
    writeBytes(root / "readme.txt", "synthetic test bank\r\n");

    // CV: _あ _い _う _え _お, and _か with a 60 ms burst.
    const std::array<std::string_view, 5> kana{"あ", "い", "う", "え", "お"};
    for (int v = 0; v < 5; ++v) {
        write(cv, std::string("_") + std::string(kana[static_cast<std::size_t>(v)]),
              sing({v}, 0.6, 0.0));
    }
    write(cv, "_か", sing({0}, 0.6, 0.06));
    // Shift-JIS: あ 82A0, い 82A2, う 82A4, え 82A6, お 82A8, か 82A9.
    const std::array<std::string_view, 5> sjis{"\x82\xA0", "\x82\xA2", "\x82\xA4", "\x82\xA6",
                                               "\x82\xA8"};
    std::string cvOto;
    for (const std::string_view k : sjis) {
        const std::string wav = "_" + std::string(k) + ".wav=";
        cvOto += wav + std::string(k) + ",20,40,-500,10,5\r\n";
        cvOto += wav + "- " + std::string(k) + ",20,40,-500,10,5\r\n";
        cvOto += wav + "* " + std::string(k) + ",60,100,-450,40,20\r\n";
    }
    // か: a 60 ms consonant, a fractional preutterance.
    cvOto += "_\x82\xA9.wav=\x82\xA9,0,60.5,-520.25,62.125,8.5\r\n";
    cvOto += "_\x82\xA9.wav=- \x82\xA9,0,60.5,-520.25,62.125,8.5\r\n";
    cvOto += "this line has no equals sign\r\n";
    cvOto += "_missing.wav=\x82\xA0\x82\xA0,0,10,10,10,10\r\n";
    writeBytes(cv / "oto.ini", cvOto);

    // VCV: one take of あいうえお, 0.4 s per vowel; aliases cut with negative cutoffs.
    write(vcv, "_あいうえお", sing({0, 1, 2, 3, 4}, 0.4, 0.0));
    std::string vcvOto;
    const std::string wav = "_\x82\xA0\x82\xA2\x82\xA4\x82\xA6\x82\xA8.wav=";
    vcvOto += wav + "- \x82\xA0,10,50,-300,30,10\r\n";
    vcvOto += wav + "a \x82\xA2,302.5,150.25,-300,100.75,40.5\r\n";
    vcvOto += wav + "i \x82\xA4,702.5,150.25,-300,100.75,40.5\r\n";
    vcvOto += wav + "u \x82\xA6,1102.5,150.25,-300,100.75,40.5\r\n";
    vcvOto += wav + "e \x82\xA8,1502.5,150.25,-280,100.75,40.5\r\n";
    writeBytes(vcv / "oto.ini", vcvOto);
    return root;
}

} // namespace

const fs::path& syntheticVoicebank() {
    static const fs::path kRoot = build();
    return kRoot;
}

} // namespace adx::tests
