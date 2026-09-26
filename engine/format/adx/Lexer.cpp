#include "engine/format/adx/Lexer.h"

#include <cctype>

namespace adx::format {
namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

[[nodiscard]] bool isHorizontalSpace(char c) noexcept {
    return c == ' ' || c == '\t';
}

[[nodiscard]] bool isIdentStart(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

[[nodiscard]] bool isIdentChar(char c) noexcept {
    return isIdentStart(c) || (c >= '0' && c <= '9');
}

/// Decodes one escape sequence. `index` points at the backslash and is advanced
/// past whatever was consumed.
///
/// An unrecognised escape keeps both characters rather than dropping the backslash:
/// a Windows path that slipped through unquoted is the likely cause, and silently
/// eating its separators would be the worst possible response.
void decodeEscape(std::string_view text, std::size_t& index, std::string& out, bool& unrecognised) {
    if (index + 1 >= text.size()) {
        out.push_back('\\');
        ++index;
        return;
    }
    const char next = text[index + 1];
    switch (next) {
    case '"':
    case '\\':
    case '#':
        out.push_back(next);
        break;
    case 'n':
        out.push_back('\n');
        break;
    case 't':
        out.push_back('\t');
        break;
    default:
        unrecognised = true;
        out.push_back('\\');
        out.push_back(next);
        break;
    }
    index += 2;
}

[[nodiscard]] std::string_view trimTrailing(std::string_view text) noexcept {
    std::size_t end = text.size();
    while (end > 0 && isHorizontalSpace(text[end - 1])) {
        --end;
    }
    return text.substr(0, end);
}

/// Decides what kind of line `line` is, from its comment-stripped content, and
/// fills in the parsed views that kind carries.
void classify(Line& line, std::string_view content, std::size_t commentAt,
              DiagnosticList& diagnostics) {
    const std::uint32_t number = line.number;
    if (content.empty()) {
        line.kind = commentAt == std::string_view::npos ? Line::Kind::Blank : Line::Kind::Comment;
    } else if (content.front() == '[') {
        line.kind = Line::Kind::SectionHeader;
        if (content.back() != ']') {
            diagnostics.add(code::kUnclosedSectionHeader, line.contentSpan(),
                            "section header is missing its closing ']'");
            line.kind = Line::Kind::Unknown;
        } else {
            const std::string_view inner = content.substr(1, content.size() - 2);
            const std::size_t space = inner.find_first_of(" \t");
            const std::string_view type =
                space == std::string_view::npos ? inner : inner.substr(0, space);
            std::string_view name;
            if (space != std::string_view::npos) {
                std::size_t nameStart = space;
                while (nameStart < inner.size() && isHorizontalSpace(inner[nameStart])) {
                    ++nameStart;
                }
                name = trimTrailing(inner.substr(nameStart));
            }
            if (type.empty()) {
                diagnostics.add(code::kEmptySectionType, line.contentSpan(),
                                "section header has an empty type");
                line.kind = Line::Kind::Unknown;
            } else {
                line.sectionType = std::string(type);
                line.sectionName = std::string(name);
            }
        }
    } else {
        // `KEY=value` only when the `=` follows an identifier and nothing else.
        // `PARAM env.attack=0.01` therefore stays positional, which is what puts
        // its inline key=value pairs through the token path instead.
        std::size_t scan = 0;
        if (isIdentStart(content[0])) {
            scan = 1;
            while (scan < content.size() && isIdentChar(content[scan])) {
                ++scan;
            }
            std::size_t afterKey = scan;
            while (afterKey < content.size() && isHorizontalSpace(content[afterKey])) {
                ++afterKey;
            }
            if (afterKey < content.size() && content[afterKey] == '=') {
                line.kind = Line::Kind::KeyValue;
                line.key = std::string(content.substr(0, scan));
                line.value = std::string(trimTrailing(content.substr(afterKey + 1)));
                line.keySpan = Span{.line = number,
                                    .column = line.contentColumn,
                                    .length = static_cast<std::uint32_t>(scan),
                                    .byteOffset = line.byteOffset + line.contentColumn - 1};
                const auto valueColumn =
                    static_cast<std::uint32_t>(line.contentColumn + afterKey + 1);
                line.valueSpan = Span{.line = number,
                                      .column = valueColumn,
                                      .length = static_cast<std::uint32_t>(line.value.size()),
                                      .byteOffset = line.byteOffset + valueColumn - 1};
                // A quoted value that never closes. Caught here rather than in
                // decodeValue, because it is a lexical fact about the line and
                // every reader of the value would otherwise have to notice it
                // separately.
                if (!line.value.empty() && line.value.front() == '"') {
                    bool closed = false;
                    for (std::size_t i = 1; i < line.value.size(); ++i) {
                        if (line.value[i] == '\\') {
                            ++i;
                            continue;
                        }
                        if (line.value[i] == '"') {
                            closed = true;
                            break;
                        }
                    }
                    if (!closed) {
                        diagnostics.add(code::kUnterminatedString, line.valueSpan,
                                        "unterminated quoted string");
                    }
                }
            }
        }
        if (line.kind != Line::Kind::KeyValue) {
            line.kind = Line::Kind::Positional;
        }
    }
}

} // namespace

std::size_t findCommentStart(std::string_view text) noexcept {
    bool inQuotes = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\') {
            // Skips whatever follows, which is what makes `\#` literal.
            ++i;
            continue;
        }
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (inQuotes || c != '#') {
            continue;
        }
        // The rule: a comment only begins where a token could begin. `F#5` keeps its
        // sharp because `#` there follows an alphanumeric.
        if (i == 0 || isHorizontalSpace(text[i - 1])) {
            return i;
        }
    }
    return std::string_view::npos;
}

std::vector<Line> lexLines(std::string_view text, std::string& bom, DiagnosticList& diagnostics) {
    bom.clear();
    std::uint32_t offset = 0;
    if (text.starts_with(kUtf8Bom)) {
        bom = std::string(kUtf8Bom);
        text.remove_prefix(kUtf8Bom.size());
        offset = static_cast<std::uint32_t>(kUtf8Bom.size());
    }

    std::vector<Line> lines;
    std::size_t cursor = 0;
    std::uint32_t number = 1;

    while (cursor <= text.size()) {
        const std::size_t newline = text.find('\n', cursor);
        const bool lastLine = newline == std::string_view::npos;
        const std::size_t lineEnd = lastLine ? text.size() : newline;

        std::string_view body = text.substr(cursor, lineEnd - cursor);
        std::string terminator;
        if (!lastLine) {
            if (!body.empty() && body.back() == '\r') {
                body.remove_suffix(1);
                terminator = "\r\n";
            } else {
                terminator = "\n";
            }
        }

        Line line;
        line.raw = std::string(body);
        line.terminator = terminator;
        line.number = number;
        line.byteOffset = offset + static_cast<std::uint32_t>(cursor);

        std::size_t indent = 0;
        while (indent < body.size() && isHorizontalSpace(body[indent])) {
            ++indent;
        }
        line.indent = static_cast<std::uint32_t>(indent);
        line.contentColumn = static_cast<std::uint32_t>(indent) + 1;

        const std::size_t commentAt = findCommentStart(body);
        const std::size_t contentEnd =
            commentAt == std::string_view::npos ? body.size() : commentAt;
        const std::string_view content =
            indent >= contentEnd ? std::string_view{}
                                 : trimTrailing(body.substr(indent, contentEnd - indent));
        line.content = std::string(content);

        for (const char c : content) {
            // Tab is legal whitespace and has already been consumed as indentation
            // where it was leading; anything else below 0x20 is not text.
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t') {
                diagnostics.add(code::kControlCharacter, line.contentSpan(),
                                "control character in text");
                break;
            }
        }

        classify(line, content, commentAt, diagnostics);
        lines.push_back(std::move(line));
        ++number;

        if (lastLine) {
            break;
        }
        cursor = newline + 1;
    }

    // A file ending in a newline produces a final empty line above, which would make
    // write() emit a trailing blank line the input did not have. Drop it; the
    // terminator on the line before it is what reproduces the newline.
    if (!lines.empty() && lines.back().raw.empty() && lines.back().terminator.empty() &&
        lines.size() > 1) {
        lines.pop_back();
    }

    return lines;
}

std::vector<Token> tokenize(const Line& line, DiagnosticList& diagnostics) {
    std::vector<Token> tokens;
    const std::string_view content = line.content;
    const std::uint32_t base = line.contentColumn;

    auto spanAt = [&](std::size_t start, std::size_t length) {
        return Span{.line = line.number,
                    .column = base + static_cast<std::uint32_t>(start),
                    .length = static_cast<std::uint32_t>(length),
                    .byteOffset = line.byteOffset + base - 1 + static_cast<std::uint32_t>(start)};
    };

    // Reads one atom - a quoted string or a run of bare characters - starting at
    // `index`, which is advanced past it. `stopAtEquals` is what lets `name="x y"`
    // split into a key and a quoted value.
    auto readAtom = [&](std::size_t& index, bool stopAtEquals, std::string& out, bool& wasQuoted,
                        std::size_t& atomStart) {
        atomStart = index;
        out.clear();
        wasQuoted = false;
        bool unrecognisedEscape = false;

        if (index < content.size() && content[index] == '"') {
            wasQuoted = true;
            ++index;
            bool closed = false;
            while (index < content.size()) {
                const char c = content[index];
                if (c == '\\') {
                    decodeEscape(content, index, out, unrecognisedEscape);
                    continue;
                }
                if (c == '"') {
                    ++index;
                    closed = true;
                    break;
                }
                out.push_back(c);
                ++index;
            }
            if (!closed) {
                diagnostics.add(code::kUnterminatedString, spanAt(atomStart, index - atomStart),
                                "unterminated quoted string");
            }
        } else {
            while (index < content.size()) {
                const char c = content[index];
                if (isHorizontalSpace(c)) {
                    break;
                }
                if (stopAtEquals && c == '=') {
                    break;
                }
                if (c == '\\') {
                    decodeEscape(content, index, out, unrecognisedEscape);
                    continue;
                }
                if (c == '"') {
                    // A quoted run *inside* a bare word keeps its quotes and its
                    // spaces. That is what makes `channel."Hardstyle Kick".cutoff` a
                    // single token - the path splitter still needs to see where the
                    // quoted segment began and ended, which stripping the quotes here
                    // would throw away.
                    out.push_back('"');
                    ++index;
                    bool closed = false;
                    while (index < content.size()) {
                        if (content[index] == '\\' && index + 1 < content.size()) {
                            out.push_back(content[index]);
                            out.push_back(content[index + 1]);
                            index += 2;
                            continue;
                        }
                        if (content[index] == '"') {
                            out.push_back('"');
                            ++index;
                            closed = true;
                            break;
                        }
                        out.push_back(content[index]);
                        ++index;
                    }
                    if (!closed) {
                        diagnostics.add(code::kUnterminatedString,
                                        spanAt(atomStart, index - atomStart),
                                        "unterminated quoted string");
                    }
                    continue;
                }
                out.push_back(c);
                ++index;
            }
        }

        if (unrecognisedEscape) {
            diagnostics.add(code::kBadEscape, spanAt(atomStart, index - atomStart),
                            "unrecognised escape sequence; the backslash is kept literally");
        }
    };

    std::size_t index = 0;
    while (index < content.size()) {
        if (isHorizontalSpace(content[index])) {
            ++index;
            continue;
        }

        Token token;
        std::size_t start = 0;
        readAtom(index, true, token.text, token.quoted, start);

        if (index < content.size() && content[index] == '=') {
            ++index;
            std::size_t valueStart = 0;
            readAtom(index, false, token.value, token.valueQuoted, valueStart);
            token.kind = TokenKind::KeyValue;
            token.valueSpan = spanAt(valueStart, index - valueStart);
        } else {
            token.kind = TokenKind::Word;
            token.valueSpan = spanAt(start, index - start);
        }
        token.span = spanAt(start, index - start);
        tokens.push_back(std::move(token));
    }

    return tokens;
}

std::string decodeValue(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    bool unrecognised = false;

    if (!text.empty() && text.front() == '"') {
        ++index;
        while (index < text.size() && text[index] != '"') {
            if (text[index] == '\\') {
                decodeEscape(text, index, out, unrecognised);
                continue;
            }
            out.push_back(text[index]);
            ++index;
        }
        return out;
    }

    while (index < text.size()) {
        if (text[index] == '\\') {
            decodeEscape(text, index, out, unrecognised);
            continue;
        }
        out.push_back(text[index]);
        ++index;
    }
    return out;
}

bool needsQuoting(std::string_view text) noexcept {
    if (text.empty()) {
        return true;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (isHorizontalSpace(c) || c == '"' || c == '\\') {
            return true;
        }
        if (c == '#' && (i == 0 || isHorizontalSpace(text[i - 1]))) {
            return true;
        }
        if (static_cast<unsigned char>(c) < 0x20) {
            return true;
        }
    }
    return false;
}

std::string quoteAlways(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    out.push_back('"');
    return out;
}

std::string quoteIfNeeded(std::string_view text) {
    return needsQuoting(text) ? quoteAlways(text) : std::string(text);
}

} // namespace adx::format
