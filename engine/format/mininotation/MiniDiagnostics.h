// adx-thread: main
//
// What the mini-notation compiler reports, and how a report becomes a Phase 2
// Diagnostic.
//
// The archived compiler returned one `errorColumn` (phase_7.md §4.5). The editor
// needs a span to underline, so a problem here carries a byte range into the source
// text it was compiled from, and toDiagnostics() maps that range back to the line and
// column in the `.adx` file the text came from - which is the only position a person
// can act on.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/format/adx/Diagnostics.h"

namespace adx::mini {

/// One problem, as a byte range into the compiled source text.
struct Problem {
    /// A Phase 2 diagnostic code: format::code::kMiniSyntax or kMiniUnknownStep.
    std::uint16_t code{0};
    std::uint32_t start{0};
    std::uint32_t length{0};
    std::string message;

    [[nodiscard]] friend bool operator==(const Problem&, const Problem&) noexcept = default;
};

/// Where one line of a MINI block sits in its file: the line number and the column
/// its (unindented) content starts at, both 1-based. What the parser knows and the
/// model does not store.
struct SourceLine {
    std::uint32_t line{1};
    std::uint32_t column{1};
    std::uint32_t byteOffset{0};
};

/// The text a MINI block's lines compile as: the lines joined by one space, so a
/// long pattern can be wrapped across lines without changing what it means.
[[nodiscard]] std::string joinLines(std::span<const std::string> lines);

/// Byte offset of each line's first character in joinLines(lines).
[[nodiscard]] std::vector<std::uint32_t> lineStarts(std::span<const std::string> lines);

/// Adds each problem to `out` at its position in the file. `lines` are the block's
/// lines as stored, `where` the position of each (same length).
void toDiagnostics(std::span<const Problem> problems, std::span<const std::string> lines,
                   std::span<const SourceLine> where, format::DiagnosticList& out);

} // namespace adx::mini
