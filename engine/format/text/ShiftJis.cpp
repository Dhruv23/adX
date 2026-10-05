// adx-thread: main
#include "engine/format/text/ShiftJis.h"

#include <algorithm>

namespace adx::format {
namespace {

void appendUtf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

constexpr std::uint32_t kReplacement = 0xFFFD;

[[nodiscard]] bool isLead(unsigned char byte) noexcept {
    return (byte >= 0x81 && byte <= 0x9F) || (byte >= 0xE0 && byte <= 0xFC);
}

} // namespace

bool isUtf8(std::string_view bytes) noexcept {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto byte = static_cast<unsigned char>(bytes[i]);
        std::size_t extra = 0;
        if (byte < 0x80) {
            extra = 0;
        } else if ((byte & 0xE0) == 0xC0 && byte >= 0xC2) {
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0 && byte <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= bytes.size()) {
            return false; // a sequence cut short by the end
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

TextEncoding detectEncoding(std::string_view bytes) noexcept {
    return isUtf8(bytes) ? TextEncoding::Utf8 : TextEncoding::ShiftJis;
}

std::string shiftJisToUtf8(std::string_view bytes) {
    const std::span<const std::uint32_t> table = shiftJisTable();
    std::string out;
    out.reserve(bytes.size() * 3 / 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto byte = static_cast<unsigned char>(bytes[i]);
        if (byte < 0x80) {
            out.push_back(static_cast<char>(byte)); // cp932's 0x5C is the backslash
        } else if (byte >= 0xA1 && byte <= 0xDF) {
            appendUtf8(out, 0xFF61U + (byte - 0xA1U)); // half-width katakana
        } else if (isLead(byte) && i + 1 < bytes.size()) {
            const auto code =
                static_cast<std::uint32_t>((byte << 8U) | static_cast<unsigned char>(bytes[i + 1]));
            const auto found = std::ranges::lower_bound(
                table, code << 16U, {}, [](std::uint32_t entry) { return entry & 0xFFFF0000U; });
            if (found != table.end() && (*found >> 16U) == code) {
                appendUtf8(out, *found & 0xFFFFU);
            } else {
                appendUtf8(out, kReplacement);
            }
            ++i;
        } else {
            appendUtf8(out, kReplacement);
        }
    }
    return out;
}

std::string toUtf8(std::string_view bytes) {
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.remove_prefix(3);
    }
    return detectEncoding(bytes) == TextEncoding::Utf8 ? std::string(bytes) : shiftJisToUtf8(bytes);
}

} // namespace adx::format
