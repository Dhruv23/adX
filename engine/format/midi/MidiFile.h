// adx-thread: main
//
// Standard MIDI Files, the plumbing both directions share (phase_8.md §3.8): big-endian
// chunk framing, variable-length quantities, and the song as the importer and exporter
// see it - notes, tempo, meter and markers, already in adX ticks.
//
// The conversion is exact wherever it can be. adX counts 3840 ticks to the quarter,
// which every common file division (96, 120, 192, 240, 384, 480, 960) divides, so
// tick * 3840 / division is an integer; a division that does not divide 3840 rounds to
// the nearest tick. Export writes adX's own 3840 to the quarter - the SMF limit is
// 32767 - so every tick is written exactly, and a file imported from any division
// exports and re-imports with identical note data (midi_import_roundtrip).
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace adx::format::midi {

/// What the exporter writes: one file tick per adX tick.
inline constexpr std::uint16_t kExportDivision = 3840;

struct MidiNote {
    /// adX ticks.
    std::int64_t start{0};
    std::int64_t length{0};
    std::uint8_t pitch{60};
    std::uint8_t velocity{100};

    [[nodiscard]] friend bool operator==(const MidiNote&, const MidiNote&) noexcept = default;
};

struct MidiTrack {
    /// The track-name meta event, or "MIDI Ch N".
    std::string name;
    /// 0-15. Channel 9 (MIDI channel 10) is General MIDI percussion.
    std::uint8_t channel{0};
    std::vector<MidiNote> notes;

    [[nodiscard]] bool percussion() const noexcept {
        return channel == 9;
    }
};

struct MidiTempo {
    std::int64_t at{0};
    double bpm{120.0};
};

struct MidiMeter {
    std::int64_t at{0};
    std::uint8_t numerator{4};
    std::uint8_t denominator{4};
};

struct MidiMarker {
    std::int64_t at{0};
    std::string name;
};

struct MidiSong {
    std::vector<MidiTrack> tracks;
    std::vector<MidiTempo> tempos;
    std::vector<MidiMeter> meters;
    std::vector<MidiMarker> markers;
    /// The file's own ticks per quarter, for the import report.
    std::uint16_t division{0};
};

/// Sorts a track's notes the way both directions compare them: by start, then pitch,
/// then length.
void sortNotes(std::vector<MidiNote>& notes);

/// Reads big-endian fields and variable-length quantities from a byte span, never past
/// its end.
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) noexcept : m_bytes(bytes) {}

    [[nodiscard]] bool has(std::size_t n) const noexcept {
        return m_at + n <= m_bytes.size();
    }
    [[nodiscard]] std::size_t position() const noexcept {
        return m_at;
    }
    void seek(std::size_t at) noexcept {
        m_at = std::min(at, m_bytes.size());
    }
    [[nodiscard]] std::uint8_t peek() const noexcept {
        return m_bytes[m_at];
    }
    std::uint8_t u8() noexcept {
        return m_bytes[m_at++];
    }
    std::uint16_t u16() noexcept;
    std::uint32_t u32() noexcept;
    /// Up to four bytes, seven bits each. False when it runs off the end or past four.
    bool vlq(std::uint32_t& out) noexcept;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        return m_bytes;
    }

private:
    std::span<const std::uint8_t> m_bytes;
    std::size_t m_at{0};
};

/// Appends big-endian fields and variable-length quantities.
class ByteWriter {
public:
    void u8(std::uint8_t value) {
        m_bytes.push_back(value);
    }
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void vlq(std::uint32_t value);
    void text(std::string_view value);
    void append(std::span<const std::uint8_t> bytes);
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept {
        return m_bytes;
    }

private:
    std::vector<std::uint8_t> m_bytes;
};

} // namespace adx::format::midi
