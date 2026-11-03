// adx-thread: main
#include "engine/format/midi/MidiFile.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace adx::format::midi {

void sortNotes(std::vector<MidiNote>& notes) {
    std::ranges::sort(notes, [](const MidiNote& a, const MidiNote& b) {
        return std::tie(a.start, a.pitch, a.length, a.velocity) <
               std::tie(b.start, b.pitch, b.length, b.velocity);
    });
}

std::uint16_t ByteReader::u16() noexcept {
    const auto value = static_cast<std::uint16_t>((m_bytes[m_at] << 8U) | m_bytes[m_at + 1]);
    m_at += 2;
    return value;
}

std::uint32_t ByteReader::u32() noexcept {
    const std::uint32_t value = (static_cast<std::uint32_t>(m_bytes[m_at]) << 24U) |
                                (static_cast<std::uint32_t>(m_bytes[m_at + 1]) << 16U) |
                                (static_cast<std::uint32_t>(m_bytes[m_at + 2]) << 8U) |
                                m_bytes[m_at + 3];
    m_at += 4;
    return value;
}

bool ByteReader::vlq(std::uint32_t& out) noexcept {
    out = 0;
    for (int i = 0; i < 4 && has(1); ++i) {
        const std::uint8_t byte = u8();
        out = (out << 7U) | (byte & 0x7FU);
        if ((byte & 0x80U) == 0) {
            return true;
        }
    }
    return false;
}

void ByteWriter::u16(std::uint16_t value) {
    m_bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    m_bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void ByteWriter::u32(std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        m_bytes.push_back(
            static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void ByteWriter::vlq(std::uint32_t value) {
    // Most significant group first; every byte but the last has its top bit set.
    std::array<std::uint8_t, 5> groups{};
    int count = 0;
    do {
        groups.at(static_cast<std::size_t>(count++)) = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7U;
    } while (value != 0 && count < 5);
    for (int i = count - 1; i >= 0; --i) {
        const std::uint8_t group = groups.at(static_cast<std::size_t>(i));
        m_bytes.push_back(i > 0 ? static_cast<std::uint8_t>(group | 0x80U) : group);
    }
}

void ByteWriter::text(std::string_view value) {
    m_bytes.insert(m_bytes.end(), value.begin(), value.end());
}

void ByteWriter::append(std::span<const std::uint8_t> bytes) {
    m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
}

} // namespace adx::format::midi
