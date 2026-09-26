#include "engine/format/adx/NoteName.h"

#include <array>
#include <charconv>

namespace adx::format {
namespace {

/// Semitone offset of each natural, indexed by letter minus 'A'.
constexpr std::array<int, 7> kNaturalOffsets{9, 11, 0, 2, 4, 5, 7};

constexpr std::array<std::string_view, 12> kSharpNames{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

[[nodiscard]] bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

} // namespace

bool noteNameToMidi(std::string_view text, std::uint8_t& out, std::uint32_t& badOffset) noexcept {
    badOffset = 0;
    if (text.empty()) {
        return false;
    }

    // A bare number is a MIDI note. Checked first so that "60" is never read as a
    // malformed note name.
    if (isDigit(text.front())) {
        int value = 0;
        const char* begin = text.data();
        const char* end = begin + text.size();
        const auto result = std::from_chars(begin, end, value);
        if (result.ec != std::errc{} || result.ptr != end || value < 0 || value > 127) {
            badOffset = static_cast<std::uint32_t>(result.ptr - begin);
            return false;
        }
        out = static_cast<std::uint8_t>(value);
        return true;
    }

    std::size_t index = 0;
    char letter = text[index];
    if (letter >= 'a' && letter <= 'z') {
        letter = static_cast<char>(letter - ('a' - 'A'));
    }
    if (letter < 'A' || letter > 'G') {
        return false;
    }
    int semitone = kNaturalOffsets.at(static_cast<std::size_t>(letter - 'A'));
    ++index;

    // Any number of accidentals, so Fx-style double sharps written `F##` work.
    while (index < text.size() && (text[index] == '#' || text[index] == 'b')) {
        semitone += text[index] == '#' ? 1 : -1;
        ++index;
    }

    if (index >= text.size()) {
        badOffset = static_cast<std::uint32_t>(index);
        return false;
    }

    int octave = 0;
    const char* begin = text.data() + index;
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, octave);
    if (result.ec != std::errc{} || result.ptr != end) {
        badOffset = static_cast<std::uint32_t>(result.ptr - text.data());
        return false;
    }

    const int midi = ((octave + 1) * 12) + semitone;
    if (midi < 0 || midi > 127) {
        badOffset = static_cast<std::uint32_t>(index);
        return false;
    }
    out = static_cast<std::uint8_t>(midi);
    return true;
}

std::string midiToNoteName(std::uint8_t midi) {
    const int octave = (midi / 12) - 1;
    std::string out(kSharpNames.at(midi % 12));
    out += std::to_string(octave);
    return out;
}

} // namespace adx::format
