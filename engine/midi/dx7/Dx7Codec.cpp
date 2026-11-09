// adx-thread: main
#include "engine/midi/dx7/Dx7Codec.h"

#include <algorithm>

#include "engine/instruments/dx7/Dx7Params.h"

namespace adx::dx7 {

namespace {

constexpr std::uint8_t kYamaha = 0x43;

/// Stores `value` into byte `byte`, clamped to its range; counts a clamp.
void put(Dx7Voice& v, std::size_t byte, int value, std::size_t& clamped) noexcept {
    const int top = maxOf(byte);
    if (value < 0 || value > top) {
        ++clamped;
        value = std::clamp(value, 0, top);
    }
    v.bytes[byte] = static_cast<std::uint8_t>(value);
}

[[nodiscard]] bool isVoiceHeader(std::span<const std::uint8_t> m) noexcept {
    return m.size() >= kSingleSysexBytes && m[1] == kYamaha && (m[2] & 0xF0) == 0x00 &&
           m[3] == 0x00 && m[4] == 0x01 && m[5] == 0x1B;
}

[[nodiscard]] bool isBankHeader(std::span<const std::uint8_t> m) noexcept {
    return m.size() >= kBankSysexBytes && m[1] == kYamaha && (m[2] & 0xF0) == 0x00 &&
           m[3] == 0x09 && m[4] == 0x20 && m[5] == 0x00;
}

} // namespace

std::uint8_t checksum(std::span<const std::uint8_t> data) noexcept {
    unsigned sum = 0;
    for (const std::uint8_t b : data) {
        sum += b;
    }
    return static_cast<std::uint8_t>((128U - (sum & 0x7FU)) & 0x7FU);
}

Vmem packVoice(const Dx7Voice& voice) noexcept {
    const auto& p = voice.bytes;
    Vmem out{};
    for (std::size_t op = 0; op < 6; ++op) {
        const std::size_t s = op * kOpBytes;
        const std::size_t d = op * 17;
        std::copy_n(p.begin() + static_cast<std::ptrdiff_t>(s), 11,
                    out.begin() + static_cast<std::ptrdiff_t>(d));
        out[d + 11] =
            static_cast<std::uint8_t>((p[s + kLeftCurve] & 3) | ((p[s + kRightCurve] & 3) << 2));
        out[d + 12] =
            static_cast<std::uint8_t>((p[s + kRateScaling] & 7) | ((p[s + kDetune] & 15) << 3));
        out[d + 13] =
            static_cast<std::uint8_t>((p[s + kAmpModSens] & 3) | ((p[s + kVelocitySens] & 7) << 2));
        out[d + 14] = p[s + kOutputLevel];
        out[d + 15] =
            static_cast<std::uint8_t>((p[s + kOscMode] & 1) | ((p[s + kCoarse] & 31) << 1));
        out[d + 16] = p[s + kFine];
    }
    std::copy_n(p.begin() + kPitchEgRate1, 9, out.begin() + 102); // pitch EG, algorithm
    out[111] = static_cast<std::uint8_t>((p[kFeedback] & 7) | ((p[kOscSync] & 1) << 3));
    std::copy_n(p.begin() + kLfoSpeed, 4, out.begin() + 112);
    out[116] = static_cast<std::uint8_t>((p[kLfoSync] & 1) | ((p[kLfoWave] & 7) << 1) |
                                         ((p[kLfoPms] & 7) << 4));
    std::copy_n(p.begin() + kTranspose, 11, out.begin() + 117); // transpose, name
    for (std::uint8_t& b : out) {
        b &= 0x7F;
    }
    return out;
}

std::size_t unpackVoice(std::span<const std::uint8_t, kVmemBytes> b, Dx7Voice& out) noexcept {
    std::size_t clamped = 0;
    for (std::size_t op = 0; op < 6; ++op) {
        const std::size_t s = op * 17;
        const std::size_t d = op * kOpBytes;
        for (std::size_t i = 0; i < 11; ++i) {
            put(out, d + i, b[s + i] & 0x7F, clamped);
        }
        put(out, d + kLeftCurve, b[s + 11] & 3, clamped);
        put(out, d + kRightCurve, (b[s + 11] >> 2) & 3, clamped);
        put(out, d + kRateScaling, b[s + 12] & 7, clamped);
        put(out, d + kDetune, (b[s + 12] >> 3) & 15, clamped);
        put(out, d + kAmpModSens, b[s + 13] & 3, clamped);
        put(out, d + kVelocitySens, (b[s + 13] >> 2) & 7, clamped);
        put(out, d + kOutputLevel, b[s + 14] & 0x7F, clamped);
        put(out, d + kOscMode, b[s + 15] & 1, clamped);
        put(out, d + kCoarse, (b[s + 15] >> 1) & 31, clamped);
        put(out, d + kFine, b[s + 16] & 0x7F, clamped);
    }
    for (std::size_t i = 0; i < 9; ++i) {
        put(out, kPitchEgRate1 + i, b[102 + i] & 0x7F, clamped);
    }
    put(out, kFeedback, b[111] & 7, clamped);
    put(out, kOscSync, (b[111] >> 3) & 1, clamped);
    for (std::size_t i = 0; i < 4; ++i) {
        put(out, kLfoSpeed + i, b[112 + i] & 0x7F, clamped);
    }
    put(out, kLfoSync, b[116] & 1, clamped);
    put(out, kLfoWave, (b[116] >> 1) & 7, clamped);
    put(out, kLfoPms, (b[116] >> 4) & 7, clamped);
    put(out, kTranspose, b[117] & 0x7F, clamped);
    for (std::size_t i = 0; i < kNameBytes; ++i) {
        out.bytes[kNameOffset + i] = static_cast<std::uint8_t>(b[118 + i] & 0x7F);
    }
    out.bytes[kOpEnable] = 0x3f;
    return clamped;
}

std::size_t voiceFromVced(std::span<const std::uint8_t> vced, Dx7Voice& out) noexcept {
    std::size_t clamped = 0;
    out = Dx7Voice::init();
    for (std::size_t i = 0; i < kVcedBytes && i < vced.size(); ++i) {
        if (i >= kNameOffset && i < kNameOffset + kNameBytes) {
            out.bytes[i] = static_cast<std::uint8_t>(vced[i] & 0x7F);
            continue;
        }
        put(out, i, vced[i], clamped);
    }
    out.bytes[kOpEnable] = 0x3f;
    return clamped;
}

std::vector<std::uint8_t> singleVoiceSysex(const Dx7Voice& voice, std::uint8_t channel) {
    std::vector<std::uint8_t> out = {0xF0, kYamaha, static_cast<std::uint8_t>(channel & 0x0F),
                                     0x00, 0x01,    0x1B};
    out.insert(out.end(), voice.bytes.begin(),
               voice.bytes.begin() + static_cast<std::ptrdiff_t>(kVcedBytes));
    out.push_back(checksum(std::span<const std::uint8_t>(voice.bytes.data(), kVcedBytes)));
    out.push_back(0xF7);
    return out;
}

std::vector<std::uint8_t> bankSysex(std::span<const Dx7Voice> voices, std::uint8_t channel) {
    std::vector<std::uint8_t> out = {0xF0, kYamaha, static_cast<std::uint8_t>(channel & 0x0F),
                                     0x09, 0x20,    0x00};
    out.reserve(kBankSysexBytes);
    const Dx7Voice init = Dx7Voice::init();
    for (std::size_t i = 0; i < kBankVoices; ++i) {
        const Vmem packed = packVoice(i < voices.size() ? voices[i] : init);
        out.insert(out.end(), packed.begin(), packed.end());
    }
    out.push_back(checksum(std::span<const std::uint8_t>(out).subspan(6)));
    out.push_back(0xF7);
    return out;
}

std::vector<std::uint8_t> dumpRequest(bool bank, std::uint8_t channel) {
    return {0xF0, kYamaha, static_cast<std::uint8_t>(0x20 | (channel & 0x0F)),
            static_cast<std::uint8_t>(bank ? 0x09 : 0x00), 0xF7};
}

ReadResult readVoices(std::span<const std::uint8_t> data) {
    ReadResult result;
    // A bare VMEM bank or VCED voice, without a SysEx frame.
    if (data.size() == kVmemBytes * kBankVoices && data[0] != 0xF0) {
        for (std::size_t v = 0; v < kBankVoices; ++v) {
            Dx7Voice voice = Dx7Voice::init();
            result.clamped += unpackVoice(std::span<const std::uint8_t, kVmemBytes>(
                                              data.data() + (v * kVmemBytes), kVmemBytes),
                                          voice);
            result.voices.push_back(voice);
        }
        return result;
    }
    if (data.size() == kVcedBytes && data[0] != 0xF0) {
        Dx7Voice voice;
        result.clamped += voiceFromVced(data, voice);
        result.voices.push_back(voice);
        return result;
    }
    std::size_t at = 0;
    while (at < data.size()) {
        if (data[at] != 0xF0) {
            ++result.skipped;
            ++at;
            continue;
        }
        std::size_t end = at + 1;
        while (end < data.size() && data[end] != 0xF7) {
            ++end;
        }
        if (end >= data.size()) {
            result.skipped += data.size() - at;
            break;
        }
        const std::span<const std::uint8_t> message = data.subspan(at, end - at + 1);
        if (isBankHeader(message)) {
            ++result.messages;
            const auto body = message.subspan(6, kVmemBytes * kBankVoices);
            if (checksum(body) != message[6 + body.size()]) {
                ++result.badChecksums;
            }
            for (std::size_t v = 0; v < kBankVoices; ++v) {
                Dx7Voice voice = Dx7Voice::init();
                result.clamped += unpackVoice(std::span<const std::uint8_t, kVmemBytes>(
                                                  body.data() + (v * kVmemBytes), kVmemBytes),
                                              voice);
                result.voices.push_back(voice);
            }
        } else if (isVoiceHeader(message)) {
            ++result.messages;
            const auto body = message.subspan(6, kVcedBytes);
            if (checksum(body) != message[6 + kVcedBytes]) {
                ++result.badChecksums;
            }
            Dx7Voice voice;
            result.clamped += voiceFromVced(body, voice);
            result.voices.push_back(voice);
        } else {
            result.skipped += message.size();
        }
        at = end + 1;
    }
    return result;
}

std::array<char, kNameBytes + 1> nameOf(const Dx7Voice& voice) noexcept {
    std::array<char, kNameBytes + 1> out{};
    for (std::size_t i = 0; i < kNameBytes; ++i) {
        const std::uint8_t c = voice.bytes[kNameOffset + i];
        out[i] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    return out;
}

} // namespace adx::dx7
