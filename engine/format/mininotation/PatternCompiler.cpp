// adx-thread: main
#include "engine/format/mininotation/PatternCompiler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <numeric>
#include <utility>

#include "engine/format/adx/Diagnostics.h"

namespace adx::mini {

namespace {

// --- AST -----------------------------------------------------------------------------

enum class NodeKind : std::uint8_t { kRest, kAtom, kSequence, kAlternation, kStack };

/// Weights and probabilities are decimals in the text; they are held in thousandths so
/// every boundary computed from them is integer arithmetic.
constexpr std::int64_t kMilli = 1000;

struct PatternNode {
    NodeKind kind{NodeKind::kRest};
    std::string token; // Atom only
    std::uint32_t sourceStart{0};
    std::uint32_t sourceLen{0};
    std::vector<PatternNode> children; // Sequence / Alternation / Stack

    std::int64_t weightMilli{kMilli}; // @n, the parent Sequence's weighted division
    int speedMul{1};                  // *n
    int speedDiv{1};                  // /n
    int replicate{1};                 // !n, consumed by the parent Sequence
    bool hasProbability{false};
    std::int64_t probabilityMilli{kMilli}; // ? / ?0.3
};

[[nodiscard]] std::int64_t positiveMod(std::int64_t value, std::int64_t modulus) noexcept {
    const std::int64_t r = value % modulus;
    return r < 0 ? r + modulus : r;
}

/// `start + span * num / den`, rounded to the nearest tick - the format's rounding.
[[nodiscard]] std::int64_t cut(std::int64_t start, std::int64_t span, std::int64_t num,
                               std::int64_t den) noexcept {
    return start + (((span * num) + (den / 2)) / den);
}

[[nodiscard]] bool isSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}
[[nodiscard]] bool isDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}
[[nodiscard]] bool isAlnum(char c) noexcept {
    return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
[[nodiscard]] char toLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
[[nodiscard]] char toUpper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

[[nodiscard]] bool isTokenChar(char c) noexcept {
    return isAlnum(c) || c == '#' || c == '-' || c == '.' || c == '_';
}

/// Parses `[-]digits[.digits]` at the front of `text` into thousandths. Returns the
/// characters consumed, 0 when there is no number.
std::size_t parseDecimalMilli(std::string_view text, std::int64_t& out) noexcept {
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && text[i] == '-') {
        negative = true;
        ++i;
    }
    std::int64_t whole = 0;
    std::size_t digits = 0;
    while (i < text.size() && isDigit(text[i])) {
        whole = std::min<std::int64_t>((whole * 10) + (text[i] - '0'), 1'000'000'000);
        ++i;
        ++digits;
    }
    std::int64_t fraction = 0;
    std::int64_t scale = kMilli;
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && isDigit(text[i])) {
            if (scale > 1) {
                scale /= 10;
                fraction += (text[i] - '0') * scale;
            }
            ++i;
            ++digits;
        }
    }
    if (digits == 0) {
        return 0;
    }
    out = ((whole * kMilli) + fraction) * (negative ? -1 : 1);
    return i;
}

/// Parses `[-]digits` at the front of `text`. Returns the characters consumed.
std::size_t parseInteger(std::string_view text, int& out) noexcept {
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && text[i] == '-') {
        negative = true;
        ++i;
    }
    std::int64_t value = 0;
    const std::size_t first = i;
    while (i < text.size() && isDigit(text[i])) {
        value = std::min<std::int64_t>((value * 10) + (text[i] - '0'), 1'000'000);
        ++i;
    }
    if (i == first) {
        return 0;
    }
    out = static_cast<int>(negative ? -value : value);
    return i;
}

// --- Euclidean rhythm ------------------------------------------------------------------

std::vector<bool> bjorklund(int k, int n) {
    if (k == 0) {
        return std::vector<bool>(static_cast<std::size_t>(n), false);
    }
    std::vector<std::vector<bool>> a(static_cast<std::size_t>(k), std::vector<bool>{true});
    std::vector<std::vector<bool>> b(static_cast<std::size_t>(n - k), std::vector<bool>{false});
    while (b.size() > 1) {
        const std::size_t m = std::min(a.size(), b.size());
        std::vector<std::vector<bool>> joined;
        joined.reserve(m);
        for (std::size_t i = 0; i < m; ++i) {
            std::vector<bool> both = a[i];
            both.insert(both.end(), b[i].begin(), b[i].end());
            joined.push_back(std::move(both));
        }
        std::vector<std::vector<bool>> rest;
        if (a.size() > m) {
            rest.assign(a.begin() + static_cast<std::ptrdiff_t>(m), a.end());
        } else {
            rest.assign(b.begin() + static_cast<std::ptrdiff_t>(m), b.end());
        }
        a = std::move(joined);
        b = std::move(rest);
    }
    std::vector<bool> out;
    out.reserve(static_cast<std::size_t>(n));
    for (const auto& group : a) {
        out.insert(out.end(), group.begin(), group.end());
    }
    for (const auto& group : b) {
        out.insert(out.end(), group.begin(), group.end());
    }
    return out;
}

// --- Parser ----------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::string_view src) noexcept : m_src(src) {}

    PatternNode parseTop() {
        skipWs();
        PatternNode root = parseStack("");
        skipWs();
        if (!m_failed && m_pos != m_src.size()) {
            fail("unexpected trailing input");
        }
        return root;
    }

    [[nodiscard]] bool failed() const noexcept {
        return m_failed;
    }
    [[nodiscard]] const std::string& errorMessage() const noexcept {
        return m_errorMessage;
    }
    [[nodiscard]] std::size_t errorOffset() const noexcept {
        return m_errorOffset;
    }

private:
    std::string_view m_src;
    std::size_t m_pos{0};
    bool m_failed{false};
    std::string m_errorMessage;
    std::size_t m_errorOffset{0};

    void fail(std::string message) {
        if (!m_failed) {
            m_failed = true;
            m_errorMessage = std::move(message);
            m_errorOffset = m_pos;
        }
    }

    [[nodiscard]] bool atEnd() const noexcept {
        return m_failed || m_pos >= m_src.size();
    }
    [[nodiscard]] char peek() const noexcept {
        return m_pos < m_src.size() ? m_src[m_pos] : '\0';
    }
    void skipWs() noexcept {
        while (m_pos < m_src.size() && isSpace(m_src[m_pos])) {
            ++m_pos;
        }
    }

    [[nodiscard]] bool isEnd(std::string_view endChars) const noexcept {
        if (atEnd()) {
            return true;
        }
        const char c = peek();
        if (c == ',') {
            return true; // ',' always separates stack layers
        }
        return endChars.find(c) != std::string_view::npos;
    }

    // One or more comma-separated sequences, played together when there is more than
    // one.
    PatternNode parseStack(std::string_view endChars) {
        std::vector<PatternNode> layers;
        layers.push_back(parseSequence(endChars));
        skipWs();
        while (!m_failed && !atEnd() && peek() == ',') {
            ++m_pos;
            skipWs();
            layers.push_back(parseSequence(endChars));
            skipWs();
        }
        if (layers.size() == 1) {
            return std::move(layers.front());
        }
        PatternNode stack;
        stack.kind = NodeKind::kStack;
        stack.children = std::move(layers);
        return stack;
    }

    // Space-separated items filling one span, each by its own @n weight.
    PatternNode parseSequence(std::string_view endChars) {
        PatternNode seq;
        seq.kind = NodeKind::kSequence;
        skipWs();
        while (!isEnd(endChars)) {
            PatternNode item = parseItem();
            if (m_failed) {
                break;
            }
            if (item.kind == NodeKind::kAtom && item.token == "!") {
                // A standalone "!" repeats the previous step verbatim.
                if (!seq.children.empty()) {
                    seq.children.push_back(seq.children.back());
                } else {
                    fail("'!' with no previous step to repeat");
                }
            } else {
                const int rep = std::max(1, item.replicate);
                item.replicate = 1;
                for (int i = 0; i < rep; ++i) {
                    seq.children.push_back(item);
                }
            }
            skipWs();
        }
        return seq;
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity) - one branch per modifier
    PatternNode parseItem() {
        PatternNode node = parseAtomOrGroup();
        if (m_failed) {
            return node;
        }
        // Postfix modifiers, glued to the token or group they modify: no whitespace.
        while (!atEnd()) {
            const char c = peek();
            if (c == '*') {
                ++m_pos;
                node.speedMul = std::clamp(readInt(1), 1, 1024);
            } else if (c == '/') {
                ++m_pos;
                node.speedDiv = std::clamp(readInt(1), 1, 1 << 20);
            } else if (c == '!') {
                ++m_pos;
                node.replicate = std::clamp(readInt(2), 1, 1024);
            } else if (c == '@') {
                ++m_pos;
                node.weightMilli = readMilli(kMilli);
            } else if (c == '?') {
                ++m_pos;
                node.hasProbability = true;
                node.probabilityMilli = (!atEnd() && (isDigit(peek()) || peek() == '.'))
                                            ? readMilli(kMilli / 2)
                                            : kMilli / 2;
            } else if (c == '(') {
                ++m_pos;
                const int k = readInt(0);
                skipWs();
                if (peek() == ',') {
                    ++m_pos;
                } else {
                    fail("expected ',' in euclid (k,n,r)");
                }
                skipWs();
                const int n = readInt(0);
                skipWs();
                int r = 0;
                if (peek() == ',') {
                    ++m_pos;
                    skipWs();
                    r = readInt(0);
                    skipWs();
                }
                if (peek() == ')') {
                    ++m_pos;
                } else {
                    fail("expected ')' closing euclid (k,n,r)");
                }
                if (!m_failed) {
                    node = applyEuclid(std::move(node), k, n, r);
                }
            } else {
                break;
            }
        }
        return node;
    }

    PatternNode applyEuclid(PatternNode base, int k, int n, int r) {
        if (n <= 0 || n > 1024) {
            fail("euclid n must be in 1..1024");
            return base;
        }
        const std::vector<bool> hits = euclid(k, n, r);
        PatternNode seq;
        seq.kind = NodeKind::kSequence;
        seq.children.reserve(static_cast<std::size_t>(n));
        for (const bool hit : hits) {
            if (hit) {
                seq.children.push_back(base);
            } else {
                seq.children.push_back(PatternNode{});
            }
        }
        return seq;
    }

    PatternNode parseAtomOrGroup() {
        if (atEnd()) {
            fail("unexpected end of pattern");
            return {};
        }
        const char c = peek();
        if (c == '~') {
            ++m_pos;
            return PatternNode{};
        }
        if (c == '!') {
            // A bare "!" as a fresh item - parseSequence turns it into a repeat.
            ++m_pos;
            PatternNode n;
            n.kind = NodeKind::kAtom;
            n.token = "!";
            return n;
        }
        if (c == '[') {
            ++m_pos;
            PatternNode inner = parseStack("]");
            skipWs();
            if (peek() == ']') {
                ++m_pos;
            } else {
                fail("expected ']'");
            }
            return inner;
        }
        if (c == '<') {
            ++m_pos;
            PatternNode seq = parseSequence(">");
            skipWs();
            if (peek() == '>') {
                ++m_pos;
            } else {
                fail("expected '>'");
            }
            PatternNode alt;
            alt.kind = NodeKind::kAlternation;
            alt.children = std::move(seq.children);
            return alt;
        }
        const std::size_t start = m_pos;
        while (!atEnd() && isTokenChar(peek())) {
            ++m_pos;
        }
        if (m_pos == start) {
            fail(std::string("unexpected character '") + c + "'");
            return {};
        }
        PatternNode n;
        n.kind = NodeKind::kAtom;
        n.token = std::string(m_src.substr(start, m_pos - start));
        n.sourceStart = static_cast<std::uint32_t>(start);
        n.sourceLen = static_cast<std::uint32_t>(m_pos - start);
        return n;
    }

    int readInt(int fallback) noexcept {
        int value = fallback;
        const std::size_t used = parseInteger(m_src.substr(m_pos), value);
        m_pos += used;
        return used == 0 ? fallback : value;
    }

    std::int64_t readMilli(std::int64_t fallback) noexcept {
        std::int64_t value = fallback;
        const std::size_t used = parseDecimalMilli(m_src.substr(m_pos), value);
        m_pos += used;
        return used == 0 ? fallback : value;
    }
};

// --- Step resolution -------------------------------------------------------------------

struct DrumName {
    std::string_view name;
    std::uint8_t note;
};

// General-MIDI drum shorthand: a fixed, well-known mapping rather than a per-instrument
// registry.
constexpr std::array<DrumName, 14> kDrums{{
    {.name = "bd", .note = 36},
    {.name = "sn", .note = 38},
    {.name = "sd", .note = 38},
    {.name = "cp", .note = 39},
    {.name = "rim", .note = 37},
    {.name = "hh", .note = 42},
    {.name = "oh", .note = 46},
    {.name = "cy", .note = 49},
    {.name = "cr", .note = 49},
    {.name = "rd", .note = 51},
    {.name = "lt", .note = 45},
    {.name = "mt", .note = 47},
    {.name = "ht", .note = 50},
    {.name = "perc", .note = 60},
}};

[[nodiscard]] std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), toLower);
    return out;
}

[[nodiscard]] int letterPitchClass(char letter) noexcept {
    switch (toUpper(letter)) {
    case 'C':
        return 0;
    case 'D':
        return 2;
    case 'E':
        return 4;
    case 'F':
        return 5;
    case 'G':
        return 7;
    case 'A':
        return 9;
    case 'B':
        return 11;
    default:
        return -1;
    }
}

// Note name to MIDI, case-insensitive, octave optional (4 when absent) - the archived
// compiler's rule, kept because it is what lets `c e g` mean a C major triad. False for
// anything that is not unambiguously a note name, so `bd` falls through to the drums.
bool tryNoteName(std::string_view token, std::uint8_t& out) noexcept {
    if (token.empty()) {
        return false;
    }
    int base = letterPitchClass(token[0]);
    if (base < 0) {
        return false;
    }
    std::size_t i = 1;
    if (i < token.size() && token[i] == '#') {
        ++base;
        ++i;
    } else if (i < token.size() && (token[i] == 'b' || token[i] == 'B')) {
        --base;
        ++i;
    }
    int octave = 4;
    if (i < token.size()) {
        int parsed = 0;
        const std::size_t used = parseInteger(token.substr(i), parsed);
        if (used == 0 || i + used != token.size()) {
            return false; // trailing garbage: not a note name
        }
        octave = parsed;
    }
    out = static_cast<std::uint8_t>(std::clamp(((octave + 1) * 12) + base, 0, 127));
    return true;
}

bool tryInteger(std::string_view token, int& out) noexcept {
    return !token.empty() && parseInteger(token, out) == token.size();
}

struct Scale {
    std::string_view mode;
    std::array<int, 12> steps;
    int count;
};

constexpr std::array<Scale, 8> kScales{{
    {.mode = "major", .steps = {0, 2, 4, 5, 7, 9, 11}, .count = 7},
    {.mode = "minor", .steps = {0, 2, 3, 5, 7, 8, 10}, .count = 7}, // natural minor
    {.mode = "dorian", .steps = {0, 2, 3, 5, 7, 9, 10}, .count = 7},
    {.mode = "phrygian", .steps = {0, 1, 3, 5, 7, 8, 10}, .count = 7},
    {.mode = "lydian", .steps = {0, 2, 4, 6, 7, 9, 11}, .count = 7},
    {.mode = "mixolydian", .steps = {0, 2, 4, 5, 7, 9, 10}, .count = 7},
    {.mode = "locrian", .steps = {0, 1, 3, 5, 6, 8, 10}, .count = 7},
    {.mode = "chromatic", .steps = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, .count = 12},
}};

// `<root>[#|b]_<mode>`, e.g. "a_minor", "c#_dorian"; the root is in octave 4 (C4 = 60).
// An unknown mode is major, as in the archive.
std::uint8_t scaleDegree(int degree, std::string_view scaleName) {
    const std::size_t underscore = scaleName.find('_');
    const std::string_view root = scaleName.substr(0, underscore);
    const std::string mode =
        underscore == std::string_view::npos ? "major" : lowered(scaleName.substr(underscore + 1));
    int rootClass = 0;
    if (!root.empty()) {
        rootClass = std::max(letterPitchClass(root[0]), 0);
        if (root.size() > 1) {
            if (root[1] == '#') {
                ++rootClass;
            } else if (root[1] == 'b' || root[1] == 'B') {
                --rootClass;
            }
        }
    }
    const Scale* scale = kScales.data();
    for (const Scale& candidate : kScales) {
        if (candidate.mode == mode) {
            scale = &candidate;
        }
    }
    const int len = scale->count;
    const int octave = degree >= 0 ? degree / len : -(((-degree) + len - 1) / len);
    const int index = degree - (octave * len);
    const int midi =
        60 + rootClass + (octave * 12) + scale->steps.at(static_cast<std::size_t>(index));
    return static_cast<std::uint8_t>(std::clamp(midi, 0, 127));
}

/// The pitch of a step token, or false when it is not a degree, a drum or a note name
/// (the caller uses middle C and reports it, as the archive fell back to middle C).
bool resolveToken(std::string_view token, std::string_view scaleName, std::uint8_t& out) {
    int degree = 0;
    if (!scaleName.empty() && tryInteger(token, degree)) {
        out = scaleDegree(degree, scaleName);
        return true;
    }
    const std::string lower = lowered(token);
    for (const DrumName& drum : kDrums) {
        if (drum.name == lower) {
            out = drum.note;
            return true;
        }
    }
    return tryNoteName(token, out);
}

/// Reports every atom that does not resolve - once, whichever cycle selects it.
void checkAtoms(const PatternNode& node, std::string_view scaleName,
                std::vector<Problem>& problems) {
    if (node.kind == NodeKind::kAtom) {
        std::uint8_t pitch = 0;
        if (!resolveToken(node.token, scaleName, pitch)) {
            const bool seen = std::ranges::any_of(
                problems, [&](const Problem& p) { return p.start == node.sourceStart; });
            if (!seen) {
                problems.push_back(Problem{.code = format::code::kMiniUnknownStep,
                                           .start = node.sourceStart,
                                           .length = node.sourceLen,
                                           .message = "'" + node.token +
                                                      "' is not a note, drum or scale degree; "
                                                      "middle C is used"});
            }
        }
        return;
    }
    for (const PatternNode& child : node.children) {
        checkAtoms(child, scaleName, problems);
    }
}

// --- Flatten (AST -> notes) ------------------------------------------------------------

struct FlattenCtx {
    std::int64_t cycleIndex{0};
    std::uint32_t rng{1};
    std::string scaleName;
    std::vector<MiniNote>* notes{nullptr};
    std::vector<StepSpan>* spans{nullptr};
};

/// xorshift32. The state must stay non-zero.
std::int64_t nextRandomMilli(std::uint32_t& state) noexcept {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(state) * kMilli) >> 32U);
}

void flattenNode(const PatternNode& node, std::int64_t start, std::int64_t span, FlattenCtx& ctx);

void flattenKind(const PatternNode& node, std::int64_t start, std::int64_t span, FlattenCtx& ctx) {
    switch (node.kind) {
    case NodeKind::kRest:
        return;
    case NodeKind::kAtom: {
        if (node.hasProbability && nextRandomMilli(ctx.rng) >= node.probabilityMilli) {
            return;
        }
        std::uint8_t pitch = 60;
        static_cast<void>(resolveToken(node.token, ctx.scaleName, pitch));
        ctx.notes->push_back(MiniNote{.start = core::Ticks{start},
                                      .length = core::Ticks{span},
                                      .pitch = pitch,
                                      .velocity = 100});
        ctx.spans->push_back(StepSpan{.start = node.sourceStart, .length = node.sourceLen});
        return;
    }
    case NodeKind::kSequence: {
        if (node.children.empty()) {
            return;
        }
        std::int64_t total = 0;
        for (const PatternNode& child : node.children) {
            total += std::max<std::int64_t>(1, child.weightMilli);
        }
        std::int64_t cumulative = 0;
        for (const PatternNode& child : node.children) {
            const std::int64_t a = cut(start, span, cumulative, total);
            cumulative += std::max<std::int64_t>(1, child.weightMilli);
            const std::int64_t b = cut(start, span, cumulative, total);
            flattenNode(child, a, b - a, ctx);
        }
        return;
    }
    case NodeKind::kAlternation: {
        if (node.children.empty()) {
            return;
        }
        const auto count = static_cast<std::int64_t>(node.children.size());
        flattenNode(node.children[static_cast<std::size_t>(positiveMod(ctx.cycleIndex, count))],
                    start, span, ctx);
        return;
    }
    case NodeKind::kStack:
        for (const PatternNode& child : node.children) {
            flattenNode(child, start, span, ctx);
        }
        return;
    }
}

void flattenNode(const PatternNode& node, std::int64_t start, std::int64_t span, FlattenCtx& ctx) {
    if (span <= 0) {
        return;
    }
    if (node.speedDiv > 1 && positiveMod(ctx.cycleIndex, node.speedDiv) != 0) {
        return; // silent this cycle
    }
    const int mul = std::max(1, node.speedMul);
    for (int i = 0; i < mul; ++i) {
        const std::int64_t a = cut(start, span, i, mul);
        const std::int64_t b = cut(start, span, i + 1, mul);
        flattenKind(node, a, b - a, ctx);
    }
}

// --- Pipe stages -----------------------------------------------------------------------

struct Range {
    std::size_t start{0};
    std::size_t end{0};
};

[[nodiscard]] Range trimmed(std::string_view text, Range r) noexcept {
    while (r.start < r.end && isSpace(text[r.start])) {
        ++r.start;
    }
    while (r.end > r.start && isSpace(text[r.end - 1])) {
        --r.end;
    }
    return r;
}

// '|' is not a character of the mini-notation grammar itself, so a top-level split is
// safe without tracking brackets.
std::vector<Range> splitStages(std::string_view text) {
    std::vector<Range> stages;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '|') {
            stages.push_back(Range{.start = start, .end = i});
            start = i + 1;
        }
    }
    stages.push_back(Range{.start = start, .end = text.size()});
    return stages;
}

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && isSpace(text[i])) {
            ++i;
        }
        const std::size_t start = i;
        while (i < text.size() && !isSpace(text[i])) {
            ++i;
        }
        if (i > start) {
            out.push_back(text.substr(start, i - start));
        }
    }
    return out;
}

struct Transform {
    enum class Kind : std::uint8_t { kRev, kFast, kEveryRev } kind{Kind::kRev};
    int everyN{1};
    std::int64_t factorMilli{kMilli};
};

/// One stage after the body. `scale=` is consumed into `scaleName` rather than becoming
/// a transform: degrees resolve while flattening, not after. Pipe stages are typed
/// syntax, so anything unrecognised is an error rather than skipped.
bool parseStage(std::string_view stage, std::string& scaleName, std::vector<Transform>& out,
                std::string& error) {
    if (stage.empty()) {
        return true; // a stray "||" or a trailing "|"
    }
    if (stage.starts_with("scale=")) {
        scaleName = std::string(stage.substr(6));
        return true;
    }
    const std::vector<std::string_view> parts = words(stage);
    if (parts.size() == 1 && parts[0] == "rev") {
        out.push_back(Transform{.kind = Transform::Kind::kRev});
        return true;
    }
    if (!parts.empty() && parts[0] == "fast") {
        std::int64_t factor = 0;
        if (parts.size() != 2 || parseDecimalMilli(parts[1], factor) != parts[1].size()) {
            error = "malformed 'fast' transform (expected 'fast <n>')";
            return false;
        }
        out.push_back(Transform{.kind = Transform::Kind::kFast, .factorMilli = factor});
        return true;
    }
    if (!parts.empty() && parts[0] == "every") {
        int n = 0;
        if (parts.size() != 3 || !tryInteger(parts[1], n)) {
            error = "malformed 'every' transform (expected 'every <n> rev')";
            return false;
        }
        if (parts[2] != "rev") {
            error = "unsupported 'every' sub-transform '" + std::string(parts[2]) +
                    "' (only 'rev' is implemented)";
            return false;
        }
        out.push_back(Transform{.kind = Transform::Kind::kEveryRev, .everyN = n});
        return true;
    }
    error = "unknown pattern transform '" + std::string(stage) +
            "' (expected scale=<name>, rev, fast <n> or every <n> rev)";
    return false;
}

// A trailing whitespace-delimited "scale=<name>" token on the body. Offsets before it
// are untouched, so step spans stay valid.
std::pair<std::string_view, std::string> splitTrailingScale(std::string_view body) {
    std::size_t end = body.size();
    while (end > 0 && isSpace(body[end - 1])) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && !isSpace(body[start - 1])) {
        --start;
    }
    const std::string_view last = body.substr(start, end - start);
    if (last.size() > 6 && last.starts_with("scale=")) {
        return {body.substr(0, start), std::string(last.substr(6))};
    }
    return {body, std::string()};
}

// Time-mirrors the cycle, so the first step plays last. Notes and spans stay paired.
void applyRev(std::vector<MiniNote>& notes, std::vector<StepSpan>& spans, std::int64_t loopStart,
              std::int64_t loopEnd) {
    for (MiniNote& n : notes) {
        n.start = core::Ticks{loopStart + (loopEnd - (n.start.value + n.length.value))};
    }
    std::ranges::reverse(notes);
    std::ranges::reverse(spans);
}

// Tiles the whole cycle round(factor) times (1..64, so `fast 999999` cannot exhaust
// memory) into the same span, each tile 1/n as long.
void applyFast(std::vector<MiniNote>& notes, std::vector<StepSpan>& spans, std::int64_t loopStart,
               std::int64_t loopEnd, std::int64_t factorMilli) {
    const auto tiles = static_cast<std::int64_t>(
        std::clamp<std::int64_t>((factorMilli + (kMilli / 2)) / kMilli, 1, 64));
    if (tiles <= 1 || notes.empty()) {
        return;
    }
    const std::int64_t length = loopEnd - loopStart;
    std::vector<MiniNote> outNotes;
    std::vector<StepSpan> outSpans;
    outNotes.reserve(notes.size() * static_cast<std::size_t>(tiles));
    outSpans.reserve(spans.size() * static_cast<std::size_t>(tiles));
    for (std::int64_t t = 0; t < tiles; ++t) {
        const std::int64_t offset = cut(loopStart, length, t, tiles);
        for (std::size_t i = 0; i < notes.size(); ++i) {
            MiniNote n = notes[i];
            n.start = core::Ticks{offset + (((n.start.value - loopStart) + (tiles / 2)) / tiles)};
            n.length =
                core::Ticks{std::max<std::int64_t>(1, (n.length.value + (tiles / 2)) / tiles)};
            outNotes.push_back(n);
            outSpans.push_back(spans[i]);
        }
    }
    notes = std::move(outNotes);
    spans = std::move(outSpans);
}

void sortByStart(std::vector<MiniNote>& notes, std::vector<StepSpan>& spans) {
    std::vector<std::size_t> order(notes.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(
        order, [&](std::size_t a, std::size_t b) { return notes[a].start < notes[b].start; });
    std::vector<MiniNote> sortedNotes;
    std::vector<StepSpan> sortedSpans;
    sortedNotes.reserve(order.size());
    sortedSpans.reserve(order.size());
    for (const std::size_t i : order) {
        sortedNotes.push_back(notes[i]);
        sortedSpans.push_back(spans[i]);
    }
    notes = std::move(sortedNotes);
    spans = std::move(sortedSpans);
}

} // namespace

core::Ticks cycleLength(const core::TempoMap& tempo) noexcept {
    const core::MeterEvent meter = tempo.meterAt(core::Ticks{0});
    return core::Ticks{static_cast<std::int64_t>(meter.numerator) *
                       core::ticksPerBeat(meter.denominator)};
}

std::uint32_t fnv1a(std::string_view text) noexcept {
    std::uint32_t h = 2166136261U;
    for (const char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619U;
    }
    return h;
}

std::vector<bool> euclid(int k, int n, int r) {
    if (n <= 0) {
        return {};
    }
    k = std::clamp(k, 0, n);
    std::vector<bool> pattern = bjorklund(k, n);
    const auto shift = static_cast<std::ptrdiff_t>(positiveMod(r, n));
    std::ranges::rotate(pattern, pattern.begin() + shift);
    return pattern;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) - the archive's one pipeline
CompileResult compile(std::string_view text, core::Ticks cycleStart, core::Ticks cycleLength,
                      std::int64_t cycleIndex, std::uint32_t seed) {
    CompileResult result;
    const std::int64_t loopStart = cycleStart.value;
    const std::int64_t loopEnd = cycleStart.value + cycleLength.value;
    if (cycleLength.value <= 0) {
        result.ok = false;
        result.problems.push_back(Problem{.code = format::code::kMiniSyntax,
                                          .start = 0,
                                          .length = static_cast<std::uint32_t>(text.size()),
                                          .message = "a cycle must be longer than zero ticks"});
        return result;
    }

    // Stage 0 is the body (which may still carry the trailing " scale=<name>" form);
    // stages 1.. are transforms or scale=.
    const std::vector<Range> stages = splitStages(text);
    std::string pipeScale;
    std::vector<Transform> transforms;
    for (std::size_t i = 1; i < stages.size(); ++i) {
        const Range stage = trimmed(text, stages[i]);
        std::string error;
        if (!parseStage(text.substr(stage.start, stage.end - stage.start), pipeScale, transforms,
                        error)) {
            result.ok = false;
            result.problems.push_back(
                Problem{.code = format::code::kMiniSyntax,
                        .start = static_cast<std::uint32_t>(stage.start),
                        .length = static_cast<std::uint32_t>(stage.end - stage.start),
                        .message = std::move(error)});
            return result;
        }
    }

    auto [body, trailingScale] = splitTrailingScale(text.substr(0, stages.front().end));
    std::string scaleName = !pipeScale.empty() ? pipeScale : trailingScale;

    Parser parser(body);
    const PatternNode root = parser.parseTop();
    if (parser.failed()) {
        result.ok = false;
        const std::size_t at = std::min(parser.errorOffset(), text.size());
        result.problems.push_back(Problem{.code = format::code::kMiniSyntax,
                                          .start = static_cast<std::uint32_t>(at),
                                          .length = at < text.size() ? 1U : 0U,
                                          .message = parser.errorMessage()});
        return result;
    }
    checkAtoms(root, scaleName, result.problems);

    FlattenCtx ctx;
    ctx.cycleIndex = cycleIndex;
    const std::uint32_t base = seed != 0 ? seed : fnv1a(text);
    // Mixed with the cycle so `?` varies from cycle to cycle, deterministically.
    std::uint32_t const state = base ^ (static_cast<std::uint32_t>(cycleIndex) * 0x9E3779B9U);
    ctx.rng = state != 0 ? state : 1U;
    ctx.scaleName = std::move(scaleName);
    ctx.notes = &result.notes;
    ctx.spans = &result.spans;
    flattenNode(root, loopStart, loopEnd - loopStart, ctx);
    sortByStart(result.notes, result.spans);

    for (const Transform& transform : transforms) {
        switch (transform.kind) {
        case Transform::Kind::kRev:
            applyRev(result.notes, result.spans, loopStart, loopEnd);
            break;
        case Transform::Kind::kFast:
            applyFast(result.notes, result.spans, loopStart, loopEnd, transform.factorMilli);
            break;
        case Transform::Kind::kEveryRev:
            if (transform.everyN > 0 && positiveMod(cycleIndex, transform.everyN) == 0) {
                applyRev(result.notes, result.spans, loopStart, loopEnd);
            }
            break;
        }
    }
    if (!transforms.empty()) {
        sortByStart(result.notes, result.spans);
    }
    return result;
}

std::vector<RealizedNote> realize(std::string_view text, core::Ticks from, core::Ticks to,
                                  core::Ticks cycleLength, std::vector<Problem>* problems) {
    std::vector<RealizedNote> out;
    if (cycleLength.value <= 0 || to <= from) {
        return out;
    }
    const std::int64_t length = cycleLength.value;
    const std::int64_t first =
        from.value >= 0 ? from.value / length : -((-from.value + length - 1) / length);
    bool reported = false;
    for (std::int64_t cycle = first; cycle * length < to.value; ++cycle) {
        CompileResult compiled = compile(text, core::Ticks{cycle * length}, cycleLength, cycle);
        if (!reported && problems != nullptr) {
            *problems = compiled.problems;
            reported = true;
        }
        if (!compiled.ok) {
            break;
        }
        for (std::size_t i = 0; i < compiled.notes.size(); ++i) {
            const MiniNote& note = compiled.notes[i];
            if (note.start < from || note.start >= to) {
                continue;
            }
            out.push_back(RealizedNote{.note = note,
                                       .span = compiled.spans[i],
                                       .cycle = cycle,
                                       .index = static_cast<std::uint32_t>(i)});
        }
    }
    return out;
}

// --- MiniDiagnostics.h -----------------------------------------------------------------

std::string joinLines(std::span<const std::string> lines) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += ' ';
        }
        out += lines[i];
    }
    return out;
}

std::vector<std::uint32_t> lineStarts(std::span<const std::string> lines) {
    std::vector<std::uint32_t> starts;
    starts.reserve(lines.size());
    std::uint32_t offset = 0;
    for (const std::string& line : lines) {
        starts.push_back(offset);
        offset += static_cast<std::uint32_t>(line.size()) + 1;
    }
    return starts;
}

void toDiagnostics(std::span<const Problem> problems, std::span<const std::string> lines,
                   std::span<const SourceLine> where, format::DiagnosticList& out) {
    if (lines.empty() || where.size() != lines.size()) {
        return;
    }
    const std::vector<std::uint32_t> starts = lineStarts(lines);
    for (const Problem& problem : problems) {
        // The line the range starts on: the last whose start is at or before it.
        std::size_t i = 0;
        while (i + 1 < starts.size() && starts[i + 1] <= problem.start) {
            ++i;
        }
        const std::uint32_t within = std::min<std::uint32_t>(
            problem.start - starts[i], static_cast<std::uint32_t>(lines[i].size()));
        const std::uint32_t length = std::min<std::uint32_t>(
            problem.length, static_cast<std::uint32_t>(lines[i].size()) - within);
        out.add(problem.code,
                format::Span{.line = where[i].line,
                             .column = where[i].column + within,
                             .length = length,
                             .byteOffset = where[i].byteOffset + within},
                problem.message);
    }
}

} // namespace adx::mini
