#include "engine/instruments/dx7/Dx7Params.h"

#include <algorithm>
#include <cmath>

#include "engine/instruments/dx7/Dx7ParamTable.h"

namespace adx::dx7 {

namespace {

constexpr std::array<std::uint8_t, kOpBytes> kOpMax = {99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                                       3,  3,  7,  3,  7,  99, 1,  31, 99, 14};

/// Bytes 126..155, by offset from 126.
constexpr std::array<std::uint8_t, 30> kVoiceMax = {
    99,  99,  99,  99,  99,  99,  99,  99,  // pitch EG
    31,  7,   1,                            // algorithm, feedback, osc sync
    99,  99,  99,  99,  1,   5,   7,        // LFO
    48,                                     // transpose
    127, 127, 127, 127, 127, 127, 127, 127, // name
    127, 127,                               //
    63};                                    // operator on/off

} // namespace

std::uint8_t maxOf(std::size_t byte) noexcept {
    if (byte < 126) {
        return kOpMax[byte % kOpBytes];
    }
    if (byte < kVoiceBytes) {
        return kVoiceMax[byte - 126];
    }
    return 0;
}

int byteOffset(std::size_t byte) noexcept {
    if (byte < 126 && byte % kOpBytes == kDetune) {
        return 7;
    }
    if (byte == kAlgorithm) {
        return -1;
    }
    if (byte == kTranspose) {
        return 24;
    }
    return 0;
}

Dx7Voice Dx7Voice::init() noexcept {
    Dx7Voice v;
    for (std::size_t i = 0; i < kVoiceBytes; ++i) {
        v.bytes[i] =
            static_cast<std::uint8_t>(std::lround(kDx7Params[i].defaultValue) + byteOffset(i));
    }
    return v;
}

std::span<const project::ParamDescriptor> dx7Params() noexcept {
    return kDx7Params;
}

void voiceFromParams(std::span<const float> params, Dx7Voice& out) noexcept {
    for (std::size_t i = 0; i < kVoiceBytes && i < params.size(); ++i) {
        const long value = std::lround(params[i]) + byteOffset(i);
        out.bytes[i] = static_cast<std::uint8_t>(std::clamp<long>(value, 0, maxOf(i)));
    }
}

void paramsFromVoice(const Dx7Voice& voice, std::span<float> params) noexcept {
    for (std::size_t i = 0; i < kVoiceBytes && i < params.size(); ++i) {
        params[i] = static_cast<float>(static_cast<int>(voice.bytes[i]) - byteOffset(i));
    }
}

} // namespace adx::dx7
