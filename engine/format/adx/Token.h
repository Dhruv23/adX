// One word of a line, with the span it came from.
//
// The format is line-oriented, so tokens exist only *within* a line and there is no
// token stream across the file. That is deliberate: it is what makes a diagnostic
// addressable as (line, column, length) without maintaining a position table, and
// what makes an unparseable line a local problem rather than a desynchronised
// parser.
#pragma once

#include <cstdint>
#include <string>

#include "engine/format/adx/Diagnostics.h"

namespace adx::format {

enum class TokenKind : std::uint8_t {
    /// A bare or quoted word: `PATTERN`, `0:0:0`, `"vocals take 3.wav"`.
    Word,
    /// `key=value`, either half of which may be quoted.
    KeyValue,
};

struct Token {
    TokenKind kind{TokenKind::Word};

    /// The decoded text - escapes resolved, surrounding quotes removed. For a
    /// KeyValue this is the key.
    std::string text;
    /// The decoded value, for a KeyValue. Empty otherwise.
    std::string value;

    /// True when the source spelling was quoted. The writer uses it to decide
    /// nothing - it re-derives quoting from the content - but a diagnostic uses it
    /// to explain why `#` did not start a comment.
    bool quoted{false};
    bool valueQuoted{false};

    /// Span of the whole token, including quotes and the `=`.
    Span span;
    /// Span of the value half of a KeyValue; equal to `span` otherwise.
    Span valueSpan;

    [[nodiscard]] bool isKeyValue() const noexcept {
        return kind == TokenKind::KeyValue;
    }
};

} // namespace adx::format
