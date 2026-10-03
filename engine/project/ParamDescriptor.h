// What a parameter is: its name, range, unit, scale and rate.
//
// Its own header, free of <string> and <vector>, because realtime code reads the
// descriptor tables: every instrument and effect declares its parameters as a
// constexpr array of these, in the order its node reads them (phase_4.md §4.2, §9:
// "a stable ParamDescriptor table per instrument and effect - the UI can generate a
// usable parameter editor generically").
#pragma once

#include <cstdint>
#include <string_view>

namespace adx::project {

enum class Unit : std::uint8_t {
    Normalized,
    Hertz,
    Decibels,
    Milliseconds,
    Seconds,
    Ratio,
    Semitones,
    Cents,
    Percent,
    Count,
    Boolean,
};

enum class ScaleKind : std::uint8_t { Linear, Logarithmic, Stepped };

/// How often a node reads a parameter (phase_4.md §4.0).
enum class RateClass : std::uint8_t {
    /// Read per sample: gain, pan, mix, drive, cutoff. Automation reaches the node as a
    /// per-frame span.
    Sample,
    /// Read on a fixed control grid - every kControlFrames frames of the node's own
    /// clock - because computing from it per sample would be wasteful (filter
    /// coefficients, delay times). Still block-size independent: the grid is the
    /// node's, not the block's.
    Block,
    /// Changes the synthesis itself and is evaluated when a note is rendered offline
    /// (the Voice instrument's flags, §4.13). Never read on the audio thread.
    Baked,
};

/// A descriptor may stand for one component of another parameter's `curve=` rather
/// than for a value of its own: how an envelope stage's shape reaches a node that only
/// ever sees floats. Such a descriptor shares its owner's name and is not automatable.
enum class CurvePart : std::uint8_t { None, Kind, Tension, C1x, C1y, C2x, C2y };

/// The control grid of RateClass::Block, in frames.
inline constexpr std::uint32_t kControlFrames = 32;

struct ParamDescriptor {
    std::string_view name;
    float minimum{0.0F};
    float maximum{1.0F};
    float defaultValue{0.0F};
    Unit unit{Unit::Normalized};
    ScaleKind scale{ScaleKind::Linear};
    RateClass rate{RateClass::Sample};
    CurvePart curve{CurvePart::None};
};

} // namespace adx::project
