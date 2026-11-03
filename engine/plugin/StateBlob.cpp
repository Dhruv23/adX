// adx-thread: main
#include "engine/plugin/StateBlob.h"

#include <array>

namespace adx::plugin {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<std::int8_t, 256> makeReverse() {
    std::array<std::int8_t, 256> table{};
    for (auto& entry : table) {
        entry = -1;
    }
    for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
        table.at(static_cast<unsigned char>(kAlphabet[i])) = static_cast<std::int8_t>(i);
    }
    return table;
}

constexpr std::array<std::int8_t, 256> kReverse = makeReverse();

} // namespace

std::string encodeBase64(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                                     (static_cast<std::uint32_t>(bytes[i + 1]) << 8U) |
                                     bytes[i + 2];
        out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
        out.push_back(kAlphabet[triple & 0x3FU]);
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t triple = static_cast<std::uint32_t>(bytes[i]) << 16U;
        out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        out += "==";
    } else if (rest == 2) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(bytes[i]) << 16U) |
                                     (static_cast<std::uint32_t>(bytes[i + 1]) << 8U);
        out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
        out.push_back(kAlphabet[(triple >> 6U) & 0x3FU]);
        out.push_back('=');
    }
    return out;
}

bool decodeBase64(std::string_view text, StateBlob& out) {
    out.clear();
    if (text.size() % 4 != 0) {
        return false;
    }
    out.reserve((text.size() / 4) * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        std::array<std::int32_t, 4> sextets{};
        std::size_t padding = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const char c = text[i + j];
            if (c == '=') {
                // Padding only in the last group's last two places.
                if (i + 4 != text.size() || j < 2) {
                    return false;
                }
                ++padding;
                sextets.at(j) = 0;
                continue;
            }
            if (padding != 0) {
                return false;
            }
            const std::int8_t v = kReverse.at(static_cast<unsigned char>(c));
            if (v < 0) {
                return false;
            }
            sextets.at(j) = static_cast<std::uint8_t>(v);
        }
        const auto triple = (static_cast<std::uint32_t>(sextets[0]) << 18U) |
                            (static_cast<std::uint32_t>(sextets[1]) << 12U) |
                            (static_cast<std::uint32_t>(sextets[2]) << 6U) |
                            static_cast<std::uint32_t>(sextets[3]);
        out.push_back(static_cast<std::uint8_t>((triple >> 16U) & 0xFFU));
        if (padding < 2) {
            out.push_back(static_cast<std::uint8_t>((triple >> 8U) & 0xFFU));
        }
        if (padding < 1) {
            out.push_back(static_cast<std::uint8_t>(triple & 0xFFU));
        }
    }
    return true;
}

} // namespace adx::plugin
