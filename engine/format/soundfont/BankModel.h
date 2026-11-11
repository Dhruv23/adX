// adx-thread: main
//
// A sample bank as SF2 and DLS both describe it (phase_12.md §4.7.4): presets by bank
// and program, each a list of zones, each zone a sample with its own key and velocity
// range, tuning, loop, envelopes, filter and LFOs. The two parsers fill this, and the
// `gm` instrument plays it, so neither format reaches the audio thread.
//
// Units are converted once, at parse time (Generators.h): seconds, decibels, Hertz and
// cents, never timecents or centibels.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

struct BankSample {
    std::string name;
    std::uint32_t rate{44100};
    std::vector<float> data;
};

struct BankPreset {
    std::uint16_t bank{0};
    std::uint8_t program{0};
    std::string name;
    std::vector<BankZone> zones;
};

/// What a parser did not apply, for the import report: never dropped silently.
struct BankNote {
    std::string what;
    std::string detail;
};

struct Bank {
    std::string name;
    std::vector<BankPreset> presets;
    std::vector<BankSample> samples;
    std::vector<BankNote> notes;

    /// GM drum kits are bank 128 in both formats as adX reads them.
    static constexpr std::uint16_t kDrumBank = 128;

    /// `bank`/`program`, else program on bank 0, else the first preset; null when empty.
    [[nodiscard]] const BankPreset* find(std::uint16_t bank, std::uint8_t program) const noexcept;
};

} // namespace adx::format::soundfont
