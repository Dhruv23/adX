// Scalar values: parsing them, and writing them back.
//
// Both directions live in one file because they have to agree exactly. A number
// that writes as `0.1` must read back as the same bits, a position that writes as
// `9:3:1920` must read back as the same tick, and the only way to be sure of that
// is for the two halves to sit next to each other where a change to one is visibly
// a change to the other.
//
// There is no std::stof here, no try, and no catch - anywhere in engine/format. A
// CI grep enforces it. The reason is not speed: std::exception::what() carries no
// position, so every one of iteration one's diagnostics degraded to "Malformed X on
// line N" with no column to underline (FINAL_PLAN §3.3.11).
//
// phase_2.md §2's manifest does not list this file; it is an addition recorded in
// that plan's §10.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/core/Rational.h"
#include "engine/core/TempoMap.h"
#include "engine/core/Time.h"
#include "engine/format/adx/Diagnostics.h"
#include "engine/project/Color.h"
#include "engine/project/Pattern.h"

namespace adx::format {

/// Narrows `span` to the characters starting at `offset`, for underlining the part
/// of a token that is actually wrong rather than the whole thing.
[[nodiscard]] Span subSpan(Span span, std::uint32_t offset, std::uint32_t length) noexcept;

/// std::from_chars, with the whole token required to be consumed. On failure a
/// diagnostic is recorded whose span starts at the first character from_chars could
/// not use.
[[nodiscard]] bool parseDouble(std::string_view text, Span span, DiagnosticList& diagnostics,
                               double& out);
[[nodiscard]] bool parseInt64(std::string_view text, Span span, DiagnosticList& diagnostics,
                              std::int64_t& out);
[[nodiscard]] bool parseBool(std::string_view text, Span span, DiagnosticList& diagnostics,
                             bool& out);

/// `bar:beat:tick`, `bar:beat`, or plain decimal beats. A token with no colon is
/// decimal beats, which is what makes `16` mean beat 16 and never bar 16.
[[nodiscard]] bool parsePosition(std::string_view text, const core::TempoMap& tempo, Span span,
                                 DiagnosticList& diagnostics, core::Ticks& out);

/// A duration is the tick distance from 0:0:0 to the same notation, so in 4/4
/// `0:1:0` is one beat and `8:0:0` is eight bars.
[[nodiscard]] bool parseDuration(std::string_view text, const core::TempoMap& tempo, Span span,
                                 DiagnosticList& diagnostics, core::Ticks& out);

/// `linear`, `exponential(0.5)`, `bezier(0.3,0,0.7,1)`, and the rest. An
/// unrecognised name is ADX1005 and `out` is left as linear.
[[nodiscard]] bool parseCurve(std::string_view text, Span span, DiagnosticList& diagnostics,
                              core::Curve& out);

/// `1/16`. Used by the arpeggiator's rate, where an exact fraction is the point.
[[nodiscard]] bool parseFraction(std::string_view text, Span span, DiagnosticList& diagnostics,
                                 core::Rational& out);

/// A pitch offset with its unit: `3st` (semitones) or `-50c` (cents). Out of the
/// int16 cents range is ADX2001.
[[nodiscard]] bool parsePitchAmount(std::string_view text, Span span, DiagnosticList& diagnostics,
                                    std::int16_t& outCents);

/// `slide=<amount>@<start>+<length>[~<curve>]`, start and length note-relative
/// durations (phase_4.md §4.0, docs/adx-format-v2.md §7.3).
[[nodiscard]] bool parseSlide(std::string_view text, const core::TempoMap& tempo, Span span,
                              DiagnosticList& diagnostics, project::NoteSlide& out);

/// `bend=<amount>@<at>[~<curve>]|...`: a note-relative pitch curve.
[[nodiscard]] bool parseBend(std::string_view text, const core::TempoMap& tempo, Span span,
                             DiagnosticList& diagnostics, std::vector<project::PitchPoint>& out);

/// `#rrggbb`, with or without the `#`.
[[nodiscard]] bool parseColor(std::string_view text, Span span, DiagnosticList& diagnostics,
                              project::Color& out);

/// Shortest round-trip representation. `0.1` stays `0.1` and never becomes
/// `0.10000000149011612`; a whole number gets a trailing `.0` only where the grammar
/// needs the token to read as a number rather than an integer.
[[nodiscard]] std::string formatDouble(double value);
[[nodiscard]] std::string formatFloat(float value);
[[nodiscard]] std::string formatPosition(core::Ticks at, const core::TempoMap& tempo);
[[nodiscard]] std::string formatDuration(core::Ticks length, const core::TempoMap& tempo);
[[nodiscard]] std::string formatCurve(const core::Curve& curve);
[[nodiscard]] std::string formatFraction(core::Rational value);
/// Semitones when the cents are a whole number of them, cents otherwise.
[[nodiscard]] std::string formatPitchAmount(std::int16_t cents);
[[nodiscard]] std::string formatSlide(const project::NoteSlide& slide, const core::TempoMap& tempo);
[[nodiscard]] std::string formatBend(const std::vector<project::PitchPoint>& points,
                                     const core::TempoMap& tempo);
[[nodiscard]] std::string formatColor(project::Color color);

[[nodiscard]] inline std::string_view formatBool(bool value) noexcept {
    return value ? "yes" : "no";
}

} // namespace adx::format
