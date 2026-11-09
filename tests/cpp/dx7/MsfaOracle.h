// Unmodified upstream msfa, driven the way its SynthUnit drives it (minus the MIDI
// parser and the resonant filter), as dx7_engine_matches_msfa's oracle. Upstream's
// headers define a global N and min/max, so they are included only in MsfaOracle.cpp.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace adx::test {

struct OracleEvent {
    enum class Kind : std::uint8_t { On, Off, Sustain, Bend };
    /// The 64-sample block before which it applies.
    int block{0};
    Kind kind{Kind::On};
    int note{60};
    int velocity{100};
    /// Sustain 0/1, or the 14-bit bend.
    int value{0};
};

/// `blocks` blocks of upstream's int32 mix for `patch` (156 bytes, unpacked).
[[nodiscard]] std::vector<std::int32_t> msfaRender(double sampleRate, const std::uint8_t* patch,
                                                   std::span<const OracleEvent> events, int blocks);

} // namespace adx::test
