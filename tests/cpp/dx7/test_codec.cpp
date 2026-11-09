// The DX7 codec, parameter changes and the parameter table (phase_11.5.md §5):
// dx7_codec_roundtrip, dx7_codec_lenient, dx7_param_change_roundtrip,
// dx7_params_cover_voice.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "engine/instruments/dx7/Dx7ParamTable.h"
#include "engine/instruments/dx7/Dx7Params.h"
#include "engine/midi/dx7/Dx7Codec.h"
#include "engine/midi/dx7/Dx7ParamChange.h"
#include "engine/project/TypeCatalog.h"

using namespace adx::dx7;

namespace {

std::vector<std::uint8_t> fixture(const char* name) {
    const std::filesystem::path path =
        std::filesystem::path(ADX_REPO_ROOT) / "tests" / "fixtures" / "dx7" / name;
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("dx7_codec_roundtrip", "[dx7]") {
    for (const char* name : {"algorithms.syx", "extremes.syx", "random.syx"}) {
        INFO(name);
        const std::vector<std::uint8_t> file = fixture(name);
        const ReadResult read = readVoices(file);
        REQUIRE(read.voices.size() == 32);
        REQUIRE(read.messages == 1);
        REQUIRE(read.badChecksums == 0);
        REQUIRE(read.clamped == 0);
        REQUIRE(read.skipped == 0);
        // Written back, the bank is the file, byte for byte (an independent Python
        // packer made the file: tests/fixtures/dx7/make_fixtures.py).
        REQUIRE(bankSysex(read.voices) == file);
        for (const Dx7Voice& voice : read.voices) {
            // VMEM -> voice -> VMEM, and VCED -> voice -> VCED.
            Dx7Voice again;
            const Vmem packed = packVoice(voice);
            REQUIRE(unpackVoice(packed, again) == 0);
            REQUIRE(again == voice);
            const auto single = singleVoiceSysex(voice);
            REQUIRE(single.size() == kSingleSysexBytes);
            const ReadResult one = readVoices(single);
            REQUIRE(one.voices.size() == 1);
            REQUIRE(one.badChecksums == 0);
            REQUIRE(one.voices[0] == voice);
        }
    }
    // The extremes bank holds every packed field at both of its ends.
    const ReadResult ext = readVoices(fixture("extremes.syx"));
    std::set<int> curves;
    std::set<int> details;
    for (const Dx7Voice& v : ext.voices) {
        curves.insert(v.bytes[Dx7Voice::opByte(1, kLeftCurve)]);
        details.insert(v.bytes[Dx7Voice::opByte(1, kDetune)]);
    }
    REQUIRE(curves == std::set<int>{0, 1, 2, 3});
    REQUIRE(details == std::set<int>{0, 14});
    // The single-voice fixture is the fifth algorithms voice.
    const ReadResult single = readVoices(fixture("single_voice.syx"));
    REQUIRE(single.voices.size() == 1);
    REQUIRE(single.voices[0] == readVoices(fixture("algorithms.syx")).voices[4]);
    // Checksums: sum plus checksum is 0 mod 128.
    const std::vector<std::uint8_t> data = {1, 2, 3, 127, 64};
    unsigned sum = 0;
    for (const auto b : data) {
        sum += b;
    }
    REQUIRE(((sum + checksum(data)) & 0x7F) == 0);
}

TEST_CASE("dx7_codec_lenient", "[dx7]") {
    SECTION("a bad checksum is reported and the voices still read") {
        const ReadResult read = readVoices(fixture("bad_checksum.syx"));
        REQUIRE(read.badChecksums == 1);
        REQUIRE(read.voices.size() == 32);
        REQUIRE(read.voices == readVoices(fixture("random.syx")).voices);
    }
    SECTION("eight banks in a row are 256 voices (sloopDX)") {
        const ReadResult read = readVoices(fixture("sloopdx_256.syx"));
        REQUIRE(read.messages == 8);
        REQUIRE(read.voices.size() == 256);
        REQUIRE(nameOf(read.voices[255]).data() == std::string("SLOOP731  "));
    }
    SECTION("out-of-range values are clamped and counted") {
        std::vector<std::uint8_t> vced(kVcedBytes, 0);
        vced[Dx7Voice::opByte(1, kDetune)] = 99; // max 14
        vced[kAlgorithm] = 40;                   // max 31
        vced[kLfoWave] = 9;                      // max 5
        vced[kNameOffset] = 0x01;                // a name byte is kept as it is
        Dx7Voice voice;
        REQUIRE(voiceFromVced(vced, voice) == 3);
        REQUIRE(voice.bytes[Dx7Voice::opByte(1, kDetune)] == 14);
        REQUIRE(voice.bytes[kAlgorithm] == 31);
        REQUIRE(voice.bytes[kLfoWave] == 5);
        REQUIRE(voice.bytes[kNameOffset] == 0x01);
        REQUIRE(nameOf(voice)[0] == '?');
        // Packed: a 4-bit detune of 15 is clamped too.
        Vmem packed = packVoice(Dx7Voice::init());
        packed[12] = static_cast<std::uint8_t>(15 << 3);
        Dx7Voice unpacked;
        REQUIRE(unpackVoice(packed, unpacked) == 1);
    }
    SECTION("bare VMEM and VCED, and junk between messages") {
        const std::vector<std::uint8_t> file = fixture("random.syx");
        const std::vector<std::uint8_t> bare(file.begin() + 6, file.end() - 2);
        REQUIRE(readVoices(bare).voices == readVoices(file).voices);
        std::vector<std::uint8_t> mixed = {0x00, 0x12};
        mixed.insert(mixed.end(), file.begin(), file.end());
        const std::vector<std::uint8_t> other = {0xF0, 0x7E, 0x00, 0x06, 0x01, 0xF7};
        mixed.insert(mixed.end(), other.begin(), other.end());
        const ReadResult read = readVoices(mixed);
        REQUIRE(read.voices.size() == 32);
        REQUIRE(read.skipped == 2 + other.size());
    }
    SECTION("dump requests") {
        REQUIRE(dumpRequest(true) == std::vector<std::uint8_t>{0xF0, 0x43, 0x20, 0x09, 0xF7});
        REQUIRE(dumpRequest(false, 3) == std::vector<std::uint8_t>{0xF0, 0x43, 0x23, 0x00, 0xF7});
    }
}

TEST_CASE("dx7_param_change_roundtrip", "[dx7]") {
    std::size_t addressed = 0;
    for (std::uint32_t param = 0; param < kDx7ParamCount; ++param) {
        const auto message = encodeParamChange({.param = param, .value = 5, .channel = 2});
        if (!message) {
            REQUIRE(param > paramIndex(Dx7Param::AftertouchAssign));
            continue;
        }
        ++addressed;
        REQUIRE((*message)[2] == 0x12);
        const auto back = decodeParamChange(*message);
        REQUIRE(back.has_value());
        const ParamChange decoded = back.value_or(ParamChange{});
        REQUIRE(decoded.param == param);
        REQUIRE(decoded.value == 5);
        REQUIRE(decoded.channel == 2);
    }
    REQUIRE(addressed == kVoiceBytes + 14);
    // The DX7's own addresses: voice parameter 135 (feedback) is group 0 with the high
    // bit; function parameter 65 is the bend range.
    const auto feedback = encodeParamChange({.param = kFeedback, .value = 7, .channel = 0});
    REQUIRE(*feedback == std::array<std::uint8_t, 7>{0xF0, 0x43, 0x10, 0x01, 0x07, 0x07, 0xF7});
    const auto bend =
        encodeParamChange({.param = paramIndex(Dx7Param::BendRange), .value = 12, .channel = 0});
    REQUIRE(*bend == std::array<std::uint8_t, 7>{0xF0, 0x43, 0x10, 0x08, 0x41, 0x0C, 0xF7});
    REQUIRE_FALSE(decodeParamChange(
        std::array<std::uint8_t, 7>{0xF0, 0x43, 0x10, 0x01, 0x7F, 0x00, 0xF7})); // 255: none
}

TEST_CASE("dx7_params_cover_voice", "[dx7]") {
    // Every voice byte is exactly one parameter and back, at both ends of its range.
    REQUIRE(kDx7Params.size() == kDx7ParamCount);
    std::set<std::string_view> names;
    for (const auto& d : kDx7Params) {
        REQUIRE(names.insert(d.name).second);
    }
    for (std::size_t byte = 0; byte < kVoiceBytes; ++byte) {
        INFO("byte " << byte << " " << kDx7Params[byte].name);
        const auto& d = kDx7Params[byte];
        REQUIRE(d.minimum == static_cast<float>(-byteOffset(byte)));
        REQUIRE(d.maximum == static_cast<float>(maxOf(byte) - byteOffset(byte)));
        for (const int raw : {0, static_cast<int>(maxOf(byte))}) {
            Dx7Voice voice = Dx7Voice::init();
            voice.bytes[byte] = static_cast<std::uint8_t>(raw);
            std::array<float, kDx7ParamCount> params{};
            paramsFromVoice(voice, params);
            REQUIRE(params[byte] == static_cast<float>(raw - byteOffset(byte)));
            Dx7Voice back;
            voiceFromParams(params, back);
            REQUIRE(back == voice);
        }
    }
    REQUIRE(kDx7Params[kAlgorithm].name == "algorithm");
    REQUIRE(kDx7Params[Dx7Voice::opByte(1, kOutputLevel)].name == "op1.level");
    REQUIRE(kDx7Params[Dx7Voice::opByte(6, kEgRate1)].name == "op6.eg.r1");
    REQUIRE(kDx7Params[paramIndex(Dx7Param::Fm1Rate)].name == "fm1.rate");
    // The defaults are INIT VOICE, and the catalog carries the table.
    const Dx7Voice init = Dx7Voice::init();
    REQUIRE(init.bytes[Dx7Voice::opByte(1, kOutputLevel)] == 99);
    REQUIRE(init.bytes[Dx7Voice::opByte(2, kOutputLevel)] == 0);
    REQUIRE(nameOf(init).data() == std::string("INIT VOICE"));
    const auto* type = adx::project::findInstrumentType("dx7");
    REQUIRE(type != nullptr);
    REQUIRE(type->params.size() == kDx7ParamCount);
}
