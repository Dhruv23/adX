// adx-thread: main
//
// The semantic token stream the editor panel highlights from (phase_7.md §4.6).
//
// The highlighter's idea of the grammar has to be the parser's, or within weeks it
// drifts - which is what the archive's LiveCodeEditor did, finding PATTERN= lines by
// string prefix. So the classification here is built on the same lexer the parser
// reads (Lexer.h): the same line kinds, the same comment rule, the same quoting; and a
// MINI block's body is classified as mini-notation because the Document says it is
// one, not because a regex guessed.
//
// Positions are byte columns within a line (0-based) - the editor maps them to its
// own string's code units, which differ only on a line with non-ASCII text.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace adx::format {

enum class SemanticKind : std::uint8_t {
    Comment,
    /// `[` and `]` of a section header.
    SectionBracket,
    SectionType,
    SectionName,
    /// The key of `KEY=value` or `key=value`.
    Key,
    /// `=`, `->`.
    Operator,
    Number,
    /// `bar:beat:tick`.
    Position,
    NoteName,
    String,
    Boolean,
    /// `insert.3`, `channel.Lead.filter.cutoff`.
    Reference,
    /// The word that opens a positional line or block: NOTES, MINI, SLOT, TRACK...
    Keyword,
    Identifier,
    // Mini-notation, inside a MINI block.
    MiniStep,
    MiniRest,
    MiniOperator,
    MiniNumber,
    /// rev, fast, every, scale=...
    MiniTransform,
    /// A line the lexer could not classify.
    Error,
};

[[nodiscard]] const char* toString(SemanticKind kind) noexcept;

struct SemanticToken {
    /// 1-based, like every other line number in the format layer.
    std::uint32_t line{1};
    /// 0-based byte offset within the line.
    std::uint32_t column{0};
    std::uint32_t length{0};
    SemanticKind kind{SemanticKind::Identifier};

    [[nodiscard]] friend bool operator==(const SemanticToken&,
                                         const SemanticToken&) noexcept = default;
};

/// Every token of `text`, in order. Whitespace is not a token. Never fails: a line the
/// lexer cannot classify is one Error token.
[[nodiscard]] std::vector<SemanticToken> semanticTokens(std::string_view text);

/// The tokens of one line, given whether it is in a MINI body. What incremental
/// highlighting re-runs for an edited line.
[[nodiscard]] std::vector<SemanticToken>
semanticTokensOfLine(std::string_view line, std::uint32_t lineNumber, bool miniBody);

} // namespace adx::format
