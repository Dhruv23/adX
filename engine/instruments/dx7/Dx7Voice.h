// A DX7 voice, unpacked: the instrument's canonical state (phase_11.5.md §4.3).
//
// The 155 VCED bytes in the DX7's own order (operator 6 first, 21 bytes each; then the
// pitch EG, algorithm, feedback, oscillator sync, LFO, transpose and the 10-byte name),
// plus byte 155, operator on/off (OP6 in bit 0 ... OP1 in bit 5), which the DX7's
// parameter-change messages also address as parameter 155. This is exactly the layout
// msfa's Dx7Note reads.
#pragma once

#include <array>
#include <cstdint>

namespace adx::dx7 {

inline constexpr std::size_t kVcedBytes = 155;
inline constexpr std::size_t kVoiceBytes = 156;
inline constexpr std::size_t kOpBytes = 21;
inline constexpr std::size_t kNameBytes = 10;
inline constexpr std::size_t kNameOffset = 145;

/// VCED field offsets within one operator's 21 bytes.
enum OpField : std::uint8_t {
    kEgRate1 = 0,
    kEgLevel1 = 4,
    kBreakPoint = 8,
    kLeftDepth = 9,
    kRightDepth = 10,
    kLeftCurve = 11,
    kRightCurve = 12,
    kRateScaling = 13,
    kAmpModSens = 14,
    kVelocitySens = 15,
    kOutputLevel = 16,
    kOscMode = 17,
    kCoarse = 18,
    kFine = 19,
    kDetune = 20,
};

/// Voice-wide VCED offsets.
enum VoiceField : std::uint8_t {
    kPitchEgRate1 = 126,
    kPitchEgLevel1 = 130,
    kAlgorithm = 134,
    kFeedback = 135,
    kOscSync = 136,
    kLfoSpeed = 137,
    kLfoDelay = 138,
    kLfoPmd = 139,
    kLfoAmd = 140,
    kLfoSync = 141,
    kLfoWave = 142,
    kLfoPms = 143,
    kTranspose = 144,
    kOpEnable = 155,
};

/// The largest value each byte may hold (the DX7's ranges).
[[nodiscard]] std::uint8_t maxOf(std::size_t byte) noexcept;

struct Dx7Voice {
    std::array<std::uint8_t, kVoiceBytes> bytes{};

    /// Byte offset of a field of operator `op` (1..6).
    [[nodiscard]] static constexpr std::size_t opByte(int op, OpField field) noexcept {
        return (static_cast<std::size_t>(6 - op) * kOpBytes) + field;
    }

    /// The DX7's INIT VOICE: operator 1 alone at full level, algorithm 1.
    [[nodiscard]] static Dx7Voice init() noexcept;

    friend bool operator==(const Dx7Voice&, const Dx7Voice&) = default;
};

} // namespace adx::dx7
