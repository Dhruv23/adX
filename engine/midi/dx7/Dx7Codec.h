// adx-thread: main
//
// DX7 SysEx voices and banks (phase_11.5.md §4.4): the 155-byte VCED single voice, the
// 32 x 128-byte packed VMEM bank, their checksums, and a lenient reader for `.syx`
// files and live dumps.
//
// Lenient means a voice from any source opens: an out-of-range value is clamped and
// counted, never rejected; a bad checksum is reported and the voices are still read
// (the librarian asks before opening them); packed and unpacked forms are both read;
// several messages in a row are read in order, so sloopDX's eight banks are 256 voices.
// Name bytes are kept as they are, whatever they hold.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/instruments/dx7/Dx7Voice.h"

namespace adx::dx7 {

inline constexpr std::size_t kVmemBytes = 128;
inline constexpr std::size_t kBankVoices = 32;
inline constexpr std::size_t kSingleSysexBytes = 6 + kVcedBytes + 2;               // 163
inline constexpr std::size_t kBankSysexBytes = 6 + (kVmemBytes * kBankVoices) + 2; // 4104

using Vmem = std::array<std::uint8_t, kVmemBytes>;

/// The DX7 checksum: the two's complement of the sum, in 7 bits.
[[nodiscard]] std::uint8_t checksum(std::span<const std::uint8_t> data) noexcept;

/// Pack to VMEM (the bank form; operator on/off is not in it).
[[nodiscard]] Vmem packVoice(const Dx7Voice& voice) noexcept;
/// Unpack VMEM; returns how many fields were out of range and clamped. Operator on/off
/// becomes all on.
std::size_t unpackVoice(std::span<const std::uint8_t, kVmemBytes> packed, Dx7Voice& out) noexcept;
/// VCED (155 bytes) into a voice, clamping; returns the count clamped.
std::size_t voiceFromVced(std::span<const std::uint8_t> vced, Dx7Voice& out) noexcept;

/// F0 43 0n 00 01 1B <155> cs F7, n the channel 0..15.
[[nodiscard]] std::vector<std::uint8_t> singleVoiceSysex(const Dx7Voice& voice,
                                                         std::uint8_t channel = 0);
/// F0 43 0n 09 20 00 <4096> cs F7. Fewer than 32 voices are padded with INIT VOICE.
[[nodiscard]] std::vector<std::uint8_t> bankSysex(std::span<const Dx7Voice> voices,
                                                  std::uint8_t channel = 0);
/// F0 43 2n 00 F7 (voice) or F0 43 2n 09 F7 (bank): a dump request.
[[nodiscard]] std::vector<std::uint8_t> dumpRequest(bool bank, std::uint8_t channel = 0);

struct ReadResult {
    std::vector<Dx7Voice> voices;
    /// Whole DX7 messages read, and how many of them had a bad checksum.
    std::size_t messages{0};
    std::size_t badChecksums{0};
    /// Values out of range, clamped.
    std::size_t clamped{0};
    /// Bytes that were not part of a DX7 voice or bank message.
    std::size_t skipped{0};
};

/// Every voice in `data`: one or more SysEx messages (single voices and banks, in
/// order), or a bare 4096-byte VMEM bank, or a bare 155-byte VCED voice.
[[nodiscard]] ReadResult readVoices(std::span<const std::uint8_t> data);

/// The name as text: bytes outside printable ASCII shown as '?', trailing spaces kept.
[[nodiscard]] std::array<char, kNameBytes + 1> nameOf(const Dx7Voice& voice) noexcept;

} // namespace adx::dx7
