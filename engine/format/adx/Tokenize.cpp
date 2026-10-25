// adx-thread: main
#include "engine/format/adx/Tokenize.h"

#include <algorithm>
#include <array>
#include <string>

#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Lexer.h"

namespace adx::format {

namespace {

[[nodiscard]] bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}
[[nodiscard]] bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t';
}

[[nodiscard]] bool isPosition(std::string_view text) noexcept {
    // [-]digits:digits:digits
    std::size_t i = text.starts_with('-') ? 1 : 0;
    for (int part = 0; part < 3; ++part) {
        const std::size_t start = i;
        while (i < text.size() && isDigit(text[i])) {
            ++i;
        }
        if (i == start) {
            return false;
        }
        if (part < 2) {
            if (i >= text.size() || text[i] != ':') {
                return false;
            }
            ++i;
        }
    }
    return i == text.size();
}

[[nodiscard]] bool isNumber(std::string_view text) noexcept {
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        ++i;
    }
    bool digits = false;
    while (i < text.size() && (isDigit(text[i]) || text[i] == '.')) {
        digits = digits || isDigit(text[i]);
        ++i;
    }
    if (!digits) {
        return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
            ++i;
        }
        while (i < text.size() && isDigit(text[i])) {
            ++i;
        }
    }
    return i == text.size();
}

[[nodiscard]] bool isNoteName(std::string_view text) noexcept {
    if (text.empty() || text[0] < 'A' || text[0] > 'G') {
        return false;
    }
    std::size_t i = 1;
    while (i < text.size() && (text[i] == '#' || text[i] == 'b')) {
        ++i;
    }
    if (i < text.size() && text[i] == '-') {
        ++i;
    }
    const std::size_t digits = i;
    while (i < text.size() && isDigit(text[i])) {
        ++i;
    }
    return i > digits && i == text.size();
}

[[nodiscard]] bool isReference(std::string_view text) noexcept {
    constexpr std::array<std::string_view, 5> kPrefixes{"insert.", "channel.", "slot.", "send.",
                                                        "master"};
    return std::ranges::any_of(
        kPrefixes, [text](std::string_view prefix) { return text.starts_with(prefix); });
}

[[nodiscard]] bool isKeywordWord(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    return std::ranges::all_of(text, [](char c) { return (c >= 'A' && c <= 'Z') || c == '_'; });
}

[[nodiscard]] SemanticKind classify(std::string_view text, bool quoted) noexcept {
    if (quoted) {
        return SemanticKind::String;
    }
    if (text == "yes" || text == "no" || text == "true" || text == "false" || text == "on" ||
        text == "off") {
        return SemanticKind::Boolean;
    }
    if (isPosition(text)) {
        return SemanticKind::Position;
    }
    if (isNumber(text)) {
        return SemanticKind::Number;
    }
    if (isNoteName(text)) {
        return SemanticKind::NoteName;
    }
    if (isReference(text)) {
        return SemanticKind::Reference;
    }
    if (text == "->") {
        return SemanticKind::Operator;
    }
    return SemanticKind::Identifier;
}

class LineTokens {
public:
    LineTokens(std::vector<SemanticToken>& out, std::uint32_t line) noexcept
        : m_out(out), m_line(line) {}

    void add(std::size_t column, std::size_t length, SemanticKind kind) {
        if (length == 0) {
            return;
        }
        m_out.push_back(SemanticToken{.line = m_line,
                                      .column = static_cast<std::uint32_t>(column),
                                      .length = static_cast<std::uint32_t>(length),
                                      .kind = kind});
    }

private:
    std::vector<SemanticToken>& m_out;
    std::uint32_t m_line;
};

/// Mini-notation, scanned the way PatternCompiler's parser reads it.
void miniTokens(std::string_view content, std::size_t base, LineTokens& out) {
    int parens = 0;
    char previous = ' ';
    std::size_t i = 0;
    while (i < content.size()) {
        const char c = content[i];
        if (isSpace(c)) {
            ++i;
            continue;
        }
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigit(c) ||
                          c == '#' || c == '-' || c == '.' || c == '_' || c == '=';
        if (!word) {
            if (c == '(') {
                ++parens;
            } else if (c == ')' && parens > 0) {
                --parens;
            }
            out.add(base + i, 1, c == '~' ? SemanticKind::MiniRest : SemanticKind::MiniOperator);
            previous = c;
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < content.size()) {
            const char d = content[i];
            const bool more = (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') || isDigit(d) ||
                              d == '#' || d == '-' || d == '.' || d == '_' || d == '=';
            if (!more) {
                break;
            }
            ++i;
        }
        const std::string_view text = content.substr(start, i - start);
        SemanticKind kind = SemanticKind::MiniStep;
        if (text == "rev" || text == "fast" || text == "every" || text.starts_with("scale=")) {
            kind = SemanticKind::MiniTransform;
        } else if (isNumber(text) && (parens > 0 || previous == '*' || previous == '/' ||
                                      previous == '!' || previous == '@' || previous == '?')) {
            kind = SemanticKind::MiniNumber;
        }
        out.add(base + start, i - start, kind);
        previous = 'a';
    }
}

void lineTokens(const Line& line, bool miniBody, std::vector<SemanticToken>& out) {
    LineTokens tokens(out, line.number);
    const std::size_t contentStart = line.contentColumn - 1;
    const std::size_t comment = findCommentStart(line.raw);

    switch (line.kind) {
    case Line::Kind::Blank:
    case Line::Kind::Comment:
        break;
    case Line::Kind::SectionHeader: {
        const std::size_t open = line.raw.find('[');
        const std::size_t close = line.raw.find(']', open == std::string::npos ? 0 : open);
        if (open == std::string::npos) {
            tokens.add(contentStart, line.content.size(), SemanticKind::Error);
            break;
        }
        tokens.add(open, 1, SemanticKind::SectionBracket);
        std::size_t at = open + 1;
        while (at < line.raw.size() && isSpace(line.raw[at])) {
            ++at;
        }
        tokens.add(at, line.sectionType.size(), SemanticKind::SectionType);
        if (!line.sectionName.empty()) {
            const std::size_t name = line.raw.find(line.sectionName, at + line.sectionType.size());
            if (name != std::string::npos) {
                tokens.add(name, line.sectionName.size(), SemanticKind::SectionName);
            }
        }
        if (close != std::string::npos) {
            tokens.add(close, 1, SemanticKind::SectionBracket);
        }
        break;
    }
    case Line::Kind::KeyValue: {
        if (miniBody) {
            miniTokens(line.content, contentStart, tokens);
            break;
        }
        const std::size_t key = line.keySpan.column - 1;
        tokens.add(key, line.keySpan.length, SemanticKind::Key);
        tokens.add(key + line.keySpan.length, 1, SemanticKind::Operator);
        const std::string_view value =
            std::string_view(line.raw).substr(line.valueSpan.column - 1, line.valueSpan.length);
        tokens.add(line.valueSpan.column - 1, line.valueSpan.length,
                   classify(line.value, value.starts_with('"')));
        break;
    }
    case Line::Kind::Positional: {
        if (miniBody) {
            miniTokens(line.content, contentStart, tokens);
            break;
        }
        DiagnosticList ignored;
        const std::vector<Token> words = tokenize(line, ignored);
        for (std::size_t i = 0; i < words.size(); ++i) {
            const Token& word = words[i];
            const std::size_t start = word.span.column - 1;
            if (word.isKeyValue()) {
                const std::size_t valueStart = word.valueSpan.column - 1;
                tokens.add(start, valueStart - 1 - start, SemanticKind::Key);
                tokens.add(valueStart - 1, 1, SemanticKind::Operator);
                tokens.add(valueStart, word.valueSpan.length,
                           classify(word.value, word.valueQuoted));
                continue;
            }
            if (i == 0 && !word.quoted && isKeywordWord(word.text)) {
                tokens.add(start, word.span.length, SemanticKind::Keyword);
                continue;
            }
            tokens.add(start, word.span.length, classify(word.text, word.quoted));
        }
        break;
    }
    case Line::Kind::Unknown:
        if (miniBody) {
            miniTokens(line.content, contentStart, tokens);
        } else {
            tokens.add(contentStart, line.content.size(), SemanticKind::Error);
        }
        break;
    }
    if (comment != std::string::npos) {
        std::size_t end = line.raw.size();
        while (end > comment && (line.raw[end - 1] == '\r' || line.raw[end - 1] == '\n')) {
            --end;
        }
        tokens.add(comment, end - comment, SemanticKind::Comment);
    }
}

[[nodiscard]] bool opensMini(const Line& line) {
    if (line.kind != Line::Kind::Positional) {
        return false;
    }
    std::string_view const content = line.content;
    return content.starts_with("MINI") && (content.size() == 4 || isSpace(content[4]));
}

} // namespace

const char* toString(SemanticKind kind) noexcept {
    switch (kind) {
    case SemanticKind::Comment:
        return "comment";
    case SemanticKind::SectionBracket:
        return "section-bracket";
    case SemanticKind::SectionType:
        return "section-type";
    case SemanticKind::SectionName:
        return "section-name";
    case SemanticKind::Key:
        return "key";
    case SemanticKind::Operator:
        return "operator";
    case SemanticKind::Number:
        return "number";
    case SemanticKind::Position:
        return "position";
    case SemanticKind::NoteName:
        return "note";
    case SemanticKind::String:
        return "string";
    case SemanticKind::Boolean:
        return "boolean";
    case SemanticKind::Reference:
        return "reference";
    case SemanticKind::Keyword:
        return "keyword";
    case SemanticKind::Identifier:
        return "identifier";
    case SemanticKind::MiniStep:
        return "mini-step";
    case SemanticKind::MiniRest:
        return "mini-rest";
    case SemanticKind::MiniOperator:
        return "mini-operator";
    case SemanticKind::MiniNumber:
        return "mini-number";
    case SemanticKind::MiniTransform:
        return "mini-transform";
    case SemanticKind::Error:
        return "error";
    }
    return "identifier";
}

std::vector<SemanticToken> semanticTokens(std::string_view text) {
    std::string bom;
    DiagnosticList ignored;
    const std::vector<Line> lines = lexLines(text, bom, ignored);
    std::vector<SemanticToken> out;
    out.reserve(lines.size() * 4);
    // A MINI block's body: the lines indented deeper than its opener, until a line
    // that is not. Blank and comment lines neither end it nor belong to it.
    bool inMini = false;
    std::uint32_t miniIndent = 0;
    for (const Line& line : lines) {
        const bool structural = line.kind != Line::Kind::Blank && line.kind != Line::Kind::Comment;
        if (inMini && structural &&
            (line.indent <= miniIndent || line.kind == Line::Kind::SectionHeader)) {
            inMini = false;
        }
        lineTokens(line, inMini && structural, out);
        if (opensMini(line)) {
            inMini = true;
            miniIndent = line.indent;
        }
    }
    return out;
}

std::vector<SemanticToken> semanticTokensOfLine(std::string_view line, std::uint32_t lineNumber,
                                                bool miniBody) {
    std::string bom;
    DiagnosticList ignored;
    std::vector<Line> lines = lexLines(line, bom, ignored);
    std::vector<SemanticToken> out;
    if (lines.empty()) {
        return out;
    }
    lines.front().number = lineNumber;
    lineTokens(lines.front(), miniBody, out);
    return out;
}

} // namespace adx::format
