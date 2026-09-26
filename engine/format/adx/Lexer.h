// Turning bytes into lines, and lines into tokens.
//
// The one non-obvious rule in this whole file, ported verbatim from iteration one
// because it got it exactly right: **`#` starts a comment, but `F#5` is a note
// name.** A `#` begins a comment only when preceded by whitespace or at the start
// of a line - never when it follows an alphanumeric. v2 adds the two cases v1 did
// not consider: a `#` inside a quoted string is literal, and a `\#` is literal.
//
// Getting this wrong silently truncates user data, which is why it has three tests
// of its own rather than being covered incidentally.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Token.h"

namespace adx::format {

/// One physical line, with every byte of it retained.
///
/// `raw` plus `terminator` reproduces the input exactly. Everything else on this
/// struct is a *view* of that text for the parser's benefit; none of it is the
/// source of truth, which is what makes Document::write() byte-identical for any
/// input including malformed ones.
struct Line {
    enum class Kind : std::uint8_t {
        Blank,
        Comment,
        SectionHeader,
        KeyValue,
        Positional,
        Unknown,
    };

    Kind kind{Kind::Blank};

    /// EXACT original bytes, without the line terminator.
    std::string raw;
    /// "\n", "\r\n", or empty for a final line with no terminator.
    std::string terminator;

    /// Line number, one-based, and the byte offset of the line's first character.
    std::uint32_t number{1};
    std::uint32_t byteOffset{0};

    /// Count of leading whitespace characters. Block structure is indentation, and
    /// this is the only thing that decides it (docs/adx-format-v2.md §4.3).
    std::uint32_t indent{0};

    /// The line with indentation and any trailing comment removed, trimmed.
    std::string content;
    /// Column, one-based, where `content` begins.
    std::uint32_t contentColumn{1};

    /// SectionHeader: the type and the verbatim argument.
    std::string sectionType;
    std::string sectionName;

    /// KeyValue: the key and the raw (still encoded) value text.
    std::string key;
    std::string value;
    Span keySpan;
    Span valueSpan;

    /// Set by the Parser when it understood this line. Anything left false is
    /// residue: reported, preserved, re-emitted (FINAL_PLAN §6 rule 4).
    bool claimed{false};

    [[nodiscard]] Span span() const noexcept {
        return Span{.line = number,
                    .column = 1,
                    .length = static_cast<std::uint32_t>(raw.size()),
                    .byteOffset = byteOffset};
    }

    /// A span covering `content`, which is what most diagnostics want to underline.
    [[nodiscard]] Span contentSpan() const noexcept {
        return Span{.line = number,
                    .column = contentColumn,
                    .length = static_cast<std::uint32_t>(content.size()),
                    .byteOffset = byteOffset + contentColumn - 1};
    }
};

/// Splits `text` into lines, classifying each. Never fails: a line it cannot
/// classify becomes Kind::Unknown with its bytes intact.
///
/// `bom` receives the UTF-8 byte order mark if the text began with one, so the
/// writer can put it back. A file that did not have one never gains one.
[[nodiscard]] std::vector<Line> lexLines(std::string_view text, std::string& bom,
                                         DiagnosticList& diagnostics);

/// Splits a line's `content` into tokens. See the file header for the `#` rule -
/// by the time this runs the comment is already gone, because lexLines removed it.
[[nodiscard]] std::vector<Token> tokenize(const Line& line, DiagnosticList& diagnostics);

/// Unquotes and unescapes one scalar value.
///
/// A `KEY=value` line already has its value separated by lexLines, but that value is
/// still in source form: `TITLE="Suffocation"` carries the quotes. This is the one
/// place that takes them off, so every section reader gets the same answer.
[[nodiscard]] std::string decodeValue(std::string_view text);

/// Where a trailing comment starts in `text`, or npos.
///
/// Exposed because the writer needs the same rule in reverse: it has to know
/// whether a string it is about to emit bare would be read back as the start of a
/// comment.
[[nodiscard]] std::size_t findCommentStart(std::string_view text) noexcept;

/// True when `text` would not survive being written without quotes - it is empty,
/// contains whitespace, a quote, a backslash, or a `#` that would start a comment.
[[nodiscard]] bool needsQuoting(std::string_view text) noexcept;

/// `text` wrapped in quotes with the necessary escapes, or `text` unchanged when it
/// does not need them.
[[nodiscard]] std::string quoteIfNeeded(std::string_view text);

/// Always quotes and escapes, for the places the grammar requires a quoted string.
[[nodiscard]] std::string quoteAlways(std::string_view text);

} // namespace adx::format
