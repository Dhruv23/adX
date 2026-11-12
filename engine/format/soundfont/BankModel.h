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

#include "engine/format/soundfont/BankZone.h"

namespace adx::format::soundfont {

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
