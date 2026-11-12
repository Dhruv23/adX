// One zone of a sample bank, as plain data (phase_12.md §4.7.4): what the `gm` voice
// reads on the audio thread. No strings or vectors here; the bank around it is
// BankModel.h, main thread only.
#pragma once

#include <cstdint>

namespace adx::format::soundfont {

/// Delay, attack, hold, decay, sustain, release. Sustain is a level 0..1; key scaling
/// stretches hold and decay per key away from middle C, as SF2's keynumTo* do.
struct BankEnvelope {
    float delay{0.0F};
    float attack{0.0F};
    float hold{0.0F};
    float decay{0.0F};
    float sustain{1.0F};
    float release{0.0F};
    /// Timecents per key from 60: hold and decay scale by 2^(-k (key - 60) / 1200).
    float keyToHold{0.0F};
    float keyToDecay{0.0F};

    [[nodiscard]] friend bool operator==(const BankEnvelope&, const BankEnvelope&) = default;
};

struct BankLfo {
    float delay{0.0F};
    float hz{8.176F};
    float toPitch{0.0F};  // cents
    float toFilter{0.0F}; // cents
    float toVolume{0.0F}; // dB

    [[nodiscard]] friend bool operator==(const BankLfo&, const BankLfo&) = default;
};

enum class BankLoop : std::uint8_t { None, Continuous, Sustain };

struct BankZone {
    std::uint32_t sample{0};
    std::uint8_t keyLow{0};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{0};
    std::uint8_t velocityHigh{127};
    std::uint8_t rootKey{60};
    /// Cents added to the note's pitch (coarse and fine tune, and the sample's own).
    float tune{0.0F};
    /// Cents per key: 100 is normal, 0 plays the same pitch on every key.
    float scaleTuning{100.0F};
    float attenuation{0.0F}; // dB, >= 0
    float pan{0.0F};         // -1..1
    /// Frames into the sample: playback range and loop, end exclusive.
    std::uint32_t start{0};
    std::uint32_t end{0};
    std::uint32_t loopStart{0};
    std::uint32_t loopEnd{0};
    BankLoop loop{BankLoop::None};
    /// Notes in the same non-zero class cut each other (a closed hat and the open one).
    std::uint16_t exclusiveClass{0};
    float filterHz{20000.0F};
    float filterQ{0.0F}; // dB of resonance
    BankEnvelope volume;
    BankEnvelope modulation;
    float modToPitch{0.0F};  // cents at full envelope
    float modToFilter{0.0F}; // cents at full envelope
    BankLfo vibrato;
    BankLfo modLfo;

    [[nodiscard]] bool matches(std::uint8_t key, std::uint8_t velocity) const noexcept {
        return key >= keyLow && key <= keyHigh && velocity >= velocityLow &&
               velocity <= velocityHigh;
    }
    [[nodiscard]] friend bool operator==(const BankZone&, const BankZone&) = default;
};

} // namespace adx::format::soundfont
