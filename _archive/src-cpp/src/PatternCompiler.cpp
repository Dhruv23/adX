#include "PatternCompiler.h"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <sstream>
#include <unordered_map>

namespace PatternCompiler {

namespace {

// --- AST ---------------------------------------------------------------

enum class NodeKind { Rest, Atom, Sequence, Alternation, Stack };

struct PatternNode {
    NodeKind kind = NodeKind::Rest;
    std::string token; // Atom only
    size_t sourceStart = 0;
    size_t sourceLen = 0;
    std::vector<PatternNode> children; // Sequence / Alternation / Stack

    float weight = 1.0f;      // @n elongate, used by the parent Sequence's weighted division
    int speedMul = 1;         // *n
    int speedDiv = 1;         // /n
    int replicate = 1;        // !n, consumed by the parent ParseSequence (never survives into flatten)
    bool hasProbability = false;
    float probability = 1.0f; // ? / ?0.3
};

// --- Euclidean rhythm ----------------------------------------------------

// Bresenham-style bucket formula, equivalent to Bjorklund's algorithm for
// all (k, n): pattern[0] fires whenever k > 0, and pattern[i] (i > 0) fires
// exactly when floor(i*k/n) differs from floor((i-1)*k/n) -- e.g. E(3,8,0)
// = x..x..x. (the canonical tresillo), matching the plan's own
// "bd(3,8,0)" acceptance example.
std::vector<bool> EuclidPattern(int k, int n, int r) {
    if (n <= 0) return {};
    k = std::clamp(k, 0, n);
    std::vector<bool> pattern(n, false);
    for (int i = 0; i < n; ++i) {
        int bucket = (i * k) / n;
        int prevBucket = (i == 0) ? -1 : ((i - 1) * k) / n;
        pattern[i] = (i == 0) ? (k > 0) : (bucket != prevBucket);
    }
    if (n > 0) {
        int shift = ((r % n) + n) % n;
        std::rotate(pattern.begin(), pattern.begin() + shift, pattern.end());
    }
    return pattern;
}

// --- Lexer/Parser ----------------------------------------------------------

bool IsTokenChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '#' || c == '-' || c == '.' || c == '_';
}

class Parser {
public:
    explicit Parser(std::string_view src) : m_src(src) {}

    PatternNode ParseTop() {
        SkipWs();
        PatternNode root = ParseStack("");
        SkipWs();
        if (!m_failed && m_pos != m_src.size()) {
            Fail("unexpected trailing input");
        }
        return root;
    }

    bool failed() const { return m_failed; }
    const std::string& errorMsg() const { return m_errorMsg; }
    size_t errorPos() const { return m_errorPos; }

private:
    std::string_view m_src;
    size_t m_pos = 0;
    bool m_failed = false;
    std::string m_errorMsg;
    size_t m_errorPos = 0;

    void Fail(const std::string& msg) {
        if (!m_failed) {
            m_failed = true;
            m_errorMsg = msg;
            m_errorPos = m_pos;
        }
    }

    bool AtEnd() const { return m_failed || m_pos >= m_src.size(); }
    char Peek() const { return m_pos < m_src.size() ? m_src[m_pos] : '\0'; }
    void SkipWs() {
        while (m_pos < m_src.size() && std::isspace(static_cast<unsigned char>(m_src[m_pos]))) m_pos++;
    }

    bool IsEnd(std::string_view endChars) {
        if (AtEnd()) return true;
        char c = Peek();
        if (c == ',') return true; // ',' always separates stack layers
        for (char e : endChars) {
            if (c == e) return true;
        }
        return false;
    }

    // One or more comma-separated ParseSequence layers, stacked (played
    // simultaneously) when there's more than one.
    PatternNode ParseStack(std::string_view endChars) {
        std::vector<PatternNode> layers;
        layers.push_back(ParseSequence(endChars));
        SkipWs();
        while (!m_failed && !AtEnd() && Peek() == ',') {
            m_pos++;
            SkipWs();
            layers.push_back(ParseSequence(endChars));
            SkipWs();
        }
        if (layers.size() == 1) return std::move(layers[0]);
        PatternNode stack;
        stack.kind = NodeKind::Stack;
        stack.children = std::move(layers);
        return stack;
    }

    // Space-separated items filling one span equally (subject to each
    // item's own @n weight).
    PatternNode ParseSequence(std::string_view endChars) {
        PatternNode seq;
        seq.kind = NodeKind::Sequence;
        SkipWs();
        while (!IsEnd(endChars)) {
            PatternNode item = ParseItem();
            if (m_failed) break;
            if (item.kind == NodeKind::Atom && item.token == "!") {
                // Standalone "!" repeats the previous step verbatim.
                if (!seq.children.empty()) seq.children.push_back(seq.children.back());
                else Fail("'!' with no previous step to repeat");
            } else {
                int rep = std::max(1, item.replicate);
                item.replicate = 1;
                for (int i = 0; i < rep; ++i) seq.children.push_back(item);
            }
            SkipWs();
        }
        return seq;
    }

    PatternNode ParseItem() {
        PatternNode node = ParseAtomOrGroup();
        if (m_failed) return node;
        // Postfix modifiers -- deliberately no whitespace skip here: they
        // must be glued directly onto the token/group they modify.
        while (!AtEnd()) {
            char c = Peek();
            if (c == '*') {
                m_pos++;
                node.speedMul = std::max(1, ParseInt(1));
            } else if (c == '/') {
                m_pos++;
                node.speedDiv = std::max(1, ParseInt(1));
            } else if (c == '!') {
                m_pos++;
                node.replicate = std::max(1, ParseInt(2));
            } else if (c == '@') {
                m_pos++;
                node.weight = ParseFloat(1.0f);
            } else if (c == '?') {
                m_pos++;
                node.hasProbability = true;
                if (!AtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '.')) {
                    node.probability = ParseFloat(0.5f);
                } else {
                    node.probability = 0.5f;
                }
            } else if (c == '(') {
                m_pos++;
                int k = ParseInt(0);
                SkipWs();
                if (Peek() == ',') m_pos++; else Fail("expected ',' in euclid(k,n,r)");
                SkipWs();
                int n = ParseInt(0);
                SkipWs();
                int r = 0;
                if (Peek() == ',') {
                    m_pos++;
                    SkipWs();
                    r = ParseInt(0);
                    SkipWs();
                }
                if (Peek() == ')') m_pos++; else Fail("expected ')' closing euclid(k,n,r)");
                if (!m_failed) node = ApplyEuclid(std::move(node), k, n, r);
            } else {
                break;
            }
        }
        return node;
    }

    PatternNode ApplyEuclid(PatternNode base, int k, int n, int r) {
        if (n <= 0) {
            Fail("euclid() n must be positive");
            return base;
        }
        std::vector<bool> hits = EuclidPattern(k, n, r);
        PatternNode seq;
        seq.kind = NodeKind::Sequence;
        seq.children.reserve(n);
        for (int i = 0; i < n; ++i) {
            if (hits[static_cast<size_t>(i)]) {
                seq.children.push_back(base);
            } else {
                PatternNode rest;
                rest.kind = NodeKind::Rest;
                seq.children.push_back(std::move(rest));
            }
        }
        return seq;
    }

    PatternNode ParseAtomOrGroup() {
        if (AtEnd()) {
            Fail("unexpected end of pattern");
            return {};
        }
        char c = Peek();
        if (c == '~') {
            m_pos++;
            PatternNode n;
            n.kind = NodeKind::Rest;
            return n;
        }
        if (c == '!') {
            // Bare "!" as a fresh item -- resolved by ParseSequence into a
            // repeat-previous-step marker.
            m_pos++;
            PatternNode n;
            n.kind = NodeKind::Atom;
            n.token = "!";
            return n;
        }
        if (c == '[') {
            m_pos++;
            PatternNode inner = ParseStack("]");
            SkipWs();
            if (Peek() == ']') m_pos++; else Fail("expected ']'");
            return inner;
        }
        if (c == '<') {
            m_pos++;
            PatternNode seq = ParseSequence(">");
            SkipWs();
            if (Peek() == '>') m_pos++; else Fail("expected '>'");
            PatternNode alt;
            alt.kind = NodeKind::Alternation;
            alt.children = std::move(seq.children);
            return alt;
        }
        size_t start = m_pos;
        while (!AtEnd() && IsTokenChar(Peek())) m_pos++;
        if (m_pos == start) {
            Fail(std::string("unexpected character '") + c + "'");
            return {};
        }
        PatternNode n;
        n.kind = NodeKind::Atom;
        n.token = std::string(m_src.substr(start, m_pos - start));
        n.sourceStart = start;
        n.sourceLen = m_pos - start;
        return n;
    }

    int ParseInt(int fallback) {
        size_t start = m_pos;
        if (!AtEnd() && Peek() == '-') m_pos++;
        while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek()))) m_pos++;
        if (m_pos == start || (m_pos == start + 1 && m_src[start] == '-')) return fallback;
        try {
            return std::stoi(std::string(m_src.substr(start, m_pos - start)));
        } catch (...) {
            return fallback;
        }
    }

    float ParseFloat(float fallback) {
        size_t start = m_pos;
        if (!AtEnd() && Peek() == '-') m_pos++;
        while (!AtEnd() && (std::isdigit(static_cast<unsigned char>(Peek())) || Peek() == '.')) m_pos++;
        if (m_pos == start) return fallback;
        try {
            return std::stof(std::string(m_src.substr(start, m_pos - start)));
        } catch (...) {
            return fallback;
        }
    }
};

// --- Step resolution -------------------------------------------------------

// General-MIDI-ish drum shorthand, the "patch-local token name" resolution
// the plan calls for -- a fixed, well-known mapping rather than a per-patch
// registry, since Patch (AudioData.h) has no drum-name field of its own.
const std::unordered_map<std::string, uint8_t>& DrumShorthand() {
    static const std::unordered_map<std::string, uint8_t> table = {
        {"bd", 36}, {"sn", 38}, {"sd", 38}, {"cp", 39}, {"rim", 37},
        {"hh", 42}, {"oh", 46}, {"cy", 49}, {"cr", 49}, {"rd", 51},
        {"lt", 45}, {"mt", 47}, {"ht", 50}, {"perc", 60},
    };
    return table;
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Note-name -> MIDI, same model as AdxParser::NoteNameToMidi (kept local
// since that method is private to AdxParser and the two formats/callers are
// independent). Returns false (rather than a default) for anything that
// isn't unambiguously a note name, so the caller can fall through to drum
// shorthand / scale-degree resolution instead of misreading e.g. "bd".
bool TryNoteNameToMidi(const std::string& tokenIn, uint8_t& outMidi) {
    if (tokenIn.empty()) return false;
    std::string t = tokenIn;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    int baseNote = 0;
    switch (t[0]) {
        case 'C': baseNote = 0; break;
        case 'D': baseNote = 2; break;
        case 'E': baseNote = 4; break;
        case 'F': baseNote = 5; break;
        case 'G': baseNote = 7; break;
        case 'A': baseNote = 9; break;
        case 'B': baseNote = 11; break;
        default: return false;
    }
    size_t i = 1;
    if (i < t.size() && t[i] == '#') { baseNote += 1; i++; }
    else if (i < t.size() && t[i] == 'B') { baseNote -= 1; i++; }

    int octave = 4;
    if (i < t.size()) {
        for (size_t j = i; j < t.size(); ++j) {
            char c = t[j];
            bool validDigit = std::isdigit(static_cast<unsigned char>(c));
            bool validSign = (j == i && c == '-');
            if (!validDigit && !validSign) return false; // trailing garbage: not a note name
        }
        try {
            octave = std::stoi(t.substr(i));
        } catch (...) {
            return false;
        }
    }
    int midi = (octave + 1) * 12 + baseNote;
    outMidi = static_cast<uint8_t>(std::clamp(midi, 0, 127));
    return true;
}

bool TryParseInt(const std::string& tok, int& out) {
    if (tok.empty()) return false;
    size_t i = (tok[0] == '-') ? 1 : 0;
    if (i >= tok.size()) return false;
    for (; i < tok.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(tok[i]))) return false;
    }
    try {
        out = std::stoi(tok);
        return true;
    } catch (...) {
        return false;
    }
}

const std::unordered_map<std::string, std::vector<int>>& ScaleIntervals() {
    static const std::unordered_map<std::string, std::vector<int>> table = {
        {"major", {0, 2, 4, 5, 7, 9, 11}},
        {"minor", {0, 2, 3, 5, 7, 8, 10}}, // natural minor
        {"dorian", {0, 2, 3, 5, 7, 9, 10}},
        {"phrygian", {0, 1, 3, 5, 7, 8, 10}},
        {"lydian", {0, 2, 4, 6, 7, 9, 11}},
        {"mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {"locrian", {0, 1, 3, 5, 6, 8, 10}},
        {"chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    };
    return table;
}

// scaleName is "<root>[#|b]_<mode>", e.g. "a_minor", "c#_dorian" -- root
// defaults to octave 4 (matches the engine's C4 == MIDI 60 convention).
uint8_t ResolveScaleDegree(int degree, const std::string& scaleName) {
    size_t underscore = scaleName.find('_');
    std::string rootStr = (underscore == std::string::npos) ? scaleName : scaleName.substr(0, underscore);
    std::string modeStr = (underscore == std::string::npos) ? "major" : scaleName.substr(underscore + 1);
    modeStr = ToLower(modeStr);

    int rootPitchClass = 0;
    if (!rootStr.empty()) {
        char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(rootStr[0])));
        switch (letter) {
            case 'C': rootPitchClass = 0; break;
            case 'D': rootPitchClass = 2; break;
            case 'E': rootPitchClass = 4; break;
            case 'F': rootPitchClass = 5; break;
            case 'G': rootPitchClass = 7; break;
            case 'A': rootPitchClass = 9; break;
            case 'B': rootPitchClass = 11; break;
            default: break;
        }
        if (rootStr.size() > 1) {
            if (rootStr[1] == '#') rootPitchClass += 1;
            else if (rootStr[1] == 'b' || rootStr[1] == 'B') rootPitchClass -= 1;
        }
    }
    int rootMidi = 60 + rootPitchClass;

    auto it = ScaleIntervals().find(modeStr);
    const std::vector<int>& intervals = (it != ScaleIntervals().end()) ? it->second : ScaleIntervals().at("major");
    int len = static_cast<int>(intervals.size());
    if (len == 0) return static_cast<uint8_t>(std::clamp(rootMidi, 0, 127));

    int octaveOffset = (degree >= 0) ? (degree / len) : -(((-degree) + len - 1) / len);
    int idx = degree - octaveOffset * len;
    if (idx < 0 || idx >= len) idx = ((idx % len) + len) % len; // defensive; shouldn't trigger given the formula above

    int midi = rootMidi + octaveOffset * 12 + intervals[static_cast<size_t>(idx)];
    return static_cast<uint8_t>(std::clamp(midi, 0, 127));
}

uint8_t ResolveToken(const std::string& token, const std::string& scaleName) {
    int degree;
    if (!scaleName.empty() && TryParseInt(token, degree)) {
        return ResolveScaleDegree(degree, scaleName);
    }
    auto dit = DrumShorthand().find(ToLower(token));
    if (dit != DrumShorthand().end()) return dit->second;
    uint8_t midi;
    if (TryNoteNameToMidi(token, midi)) return midi;
    return 60; // unresolved: fall back to Middle C, same discipline as AdxParser::NoteNameToMidi
}

// --- Flatten (AST -> Notes) -------------------------------------------------

struct FlattenCtx {
    int cycleIndex = 0;
    uint32_t rngState = 1;
    std::string scaleName;
    std::vector<Note>* notes = nullptr;
    std::vector<StepSpan>* spans = nullptr;
};

float NextRandom01(uint32_t& rng) {
    // xorshift32, same generator shape as Voice::noiseRng (AudioEngine.h) --
    // must stay non-zero.
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<float>(rng) / 4294967295.0f;
}

void FlattenNode(const PatternNode& node, float start, float span, FlattenCtx& ctx);

void FlattenKind(const PatternNode& node, float start, float span, FlattenCtx& ctx) {
    switch (node.kind) {
        case NodeKind::Rest:
            return;
        case NodeKind::Atom: {
            if (node.hasProbability) {
                float roll = NextRandom01(ctx.rngState);
                if (roll >= node.probability) return;
            }
            Note note;
            note.startBeat = start;
            note.lengthBeats = span;
            note.pitch = ResolveToken(node.token, ctx.scaleName);
            note.velocity = 100;
            ctx.notes->push_back(note);
            if (ctx.spans) ctx.spans->push_back(StepSpan{node.sourceStart, node.sourceLen});
            return;
        }
        case NodeKind::Sequence: {
            if (node.children.empty()) return;
            float totalWeight = 0.0f;
            for (const auto& c : node.children) totalWeight += std::max(0.0001f, c.weight);
            float cursor = start;
            for (const auto& c : node.children) {
                float w = std::max(0.0001f, c.weight);
                float childSpan = span * (w / totalWeight);
                FlattenNode(c, cursor, childSpan, ctx);
                cursor += childSpan;
            }
            return;
        }
        case NodeKind::Alternation: {
            if (node.children.empty()) return;
            int count = static_cast<int>(node.children.size());
            int idx = ((ctx.cycleIndex % count) + count) % count;
            FlattenNode(node.children[static_cast<size_t>(idx)], start, span, ctx);
            return;
        }
        case NodeKind::Stack: {
            for (const auto& c : node.children) FlattenNode(c, start, span, ctx);
            return;
        }
    }
}

void FlattenNode(const PatternNode& node, float start, float span, FlattenCtx& ctx) {
    if (span <= 0.0f) return;
    if (node.speedDiv > 1 && (ctx.cycleIndex % node.speedDiv) != 0) return; // silent this cycle
    int mul = std::max(1, node.speedMul);
    float tileSpan = span / static_cast<float>(mul);
    for (int i = 0; i < mul; ++i) {
        FlattenKind(node, start + static_cast<float>(i) * tileSpan, tileSpan, ctx);
    }
}

uint32_t FnvHash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

// A trailing whitespace-delimited "scale=<name>" token switches the pattern
// out of note/drum-name resolution; strip it from the tail before parsing
// (offsets for everything before it are untouched, so StepSpans stay valid).
std::pair<std::string_view, std::string> SplitTrailingScale(std::string_view text) {
    size_t end = text.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
    size_t start = end;
    while (start > 0 && !std::isspace(static_cast<unsigned char>(text[start - 1]))) start--;
    std::string_view lastTok = text.substr(start, end - start);
    if (lastTok.size() > 6 && lastTok.substr(0, 6) == "scale=") {
        return {text.substr(0, start), std::string(lastTok.substr(6))};
    }
    return {text, std::string()};
}

// --- live-PLAN L5 (stretch): pipe-separated pattern transforms -----------

std::string TrimCopy(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return std::string(s.substr(b, e - b));
}

// Splits on every top-level '|'. Safe unconditionally: '|' never appears
// inside the mini-notation grammar itself (not a token/structural/modifier
// character anywhere in the parser above), so no bracket-nesting tracking
// is needed.
std::vector<std::string> SplitPipeStages(const std::string& text) {
    std::vector<std::string> stages;
    size_t start = 0;
    size_t pos;
    while ((pos = text.find('|', start)) != std::string::npos) {
        stages.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    stages.push_back(text.substr(start));
    return stages;
}

struct ParsedTransform {
    enum class Kind { Rev, Fast, EveryRev } kind;
    int everyN = 1;
    float factor = 1.0f;
};

// Parses one pipe stage (already excluding the mini-notation body, i.e.
// stages[1..]). "scale=<name>" stages are consumed into outScaleName
// (taking precedence over L2's trailing-space form, if both are present)
// rather than becoming a transform, since scale resolution has to happen
// before flatten, not as a post-hoc Note-vector transform. Returns false
// (with errorOut set) for anything unrecognized -- pipe stages are new,
// deliberately-typed syntax, not a "skip unknown keys" format like .adx.
bool ParseTransformStage(const std::string& stageRaw, std::string& outScaleName,
                          std::vector<ParsedTransform>& outTransforms, std::string& errorOut) {
    std::string stage = TrimCopy(stageRaw);
    if (stage.empty()) return true; // tolerate a stray "||" or trailing "|"

    if (stage.rfind("scale=", 0) == 0) {
        outScaleName = stage.substr(6);
        return true;
    }
    if (stage == "rev") {
        outTransforms.push_back(ParsedTransform{ParsedTransform::Kind::Rev, 0, 0.0f});
        return true;
    }
    if (stage.rfind("fast", 0) == 0 && (stage.size() == 4 || std::isspace(static_cast<unsigned char>(stage[4])))) {
        std::istringstream iss(stage);
        std::string tok;
        float n = 1.0f;
        iss >> tok;
        if (!(iss >> n)) {
            errorOut = "malformed 'fast' transform (expected 'fast <n>'): " + stage;
            return false;
        }
        outTransforms.push_back(ParsedTransform{ParsedTransform::Kind::Fast, 0, n});
        return true;
    }
    if (stage.rfind("every", 0) == 0 && (stage.size() == 5 || std::isspace(static_cast<unsigned char>(stage[5])))) {
        std::istringstream iss(stage);
        std::string tok, sub;
        int n = 1;
        iss >> tok;
        if (!(iss >> n) || !(iss >> sub)) {
            errorOut = "malformed 'every' transform (expected 'every <n> rev'): " + stage;
            return false;
        }
        if (sub != "rev") {
            errorOut = "unsupported 'every' sub-transform (only 'rev' is implemented): " + sub;
            return false;
        }
        outTransforms.push_back(ParsedTransform{ParsedTransform::Kind::EveryRev, n, 0.0f});
        return true;
    }

    errorOut = "unknown pattern transform: '" + stage + "' (expected scale=<name>, rev, fast <n>, or every <n> rev)";
    return false;
}

// Time-mirrors the compiled cycle: the note that started closest to
// loopStartBeat ends up closest to loopEndBeat and vice versa, reversing
// audible step order. Keeps `notes`/`spans` in lockstep (same 1:1
// correspondence, just retimed + reordered) so step-highlighting (M7)
// still resolves correctly against a rev'd pattern.
void ApplyRev(std::vector<Note>& notes, std::vector<StepSpan>& spans, float loopStart, float loopEnd) {
    for (auto& n : notes) {
        n.startBeat = loopStart + (loopEnd - (n.startBeat + n.lengthBeats));
    }
    std::reverse(notes.begin(), notes.end());
    std::reverse(spans.begin(), spans.end());
}

// Tiles the whole compiled cycle `tiles` times into the same [loopStart,
// loopEnd) span (tiles = round(factor), clamped to [1, 64] so a typo like
// "fast 999999" can't blow up memory) -- each repetition compressed to
// 1/tiles the duration, so the pattern plays `tiles` times as fast without
// touching pitch. Using the rounded integer `tiles` for both the divisor
// and the loop count (rather than the raw, possibly-fractional `factor`)
// guarantees the tiles exactly cover the cycle with no gap or overflow.
void ApplyFast(std::vector<Note>& notes, std::vector<StepSpan>& spans, float loopStart, float loopEnd, float factor) {
    int tiles = std::clamp(static_cast<int>(std::lround(factor)), 1, 64);
    if (tiles <= 1 || notes.empty()) return;

    float cycleLen = loopEnd - loopStart;
    float compressedLen = cycleLen / static_cast<float>(tiles);
    std::vector<Note> outNotes;
    std::vector<StepSpan> outSpans;
    outNotes.reserve(notes.size() * static_cast<size_t>(tiles));
    outSpans.reserve(spans.size() * static_cast<size_t>(tiles));
    for (int t = 0; t < tiles; ++t) {
        float tileOffset = loopStart + static_cast<float>(t) * compressedLen;
        for (size_t i = 0; i < notes.size(); ++i) {
            Note n = notes[i];
            float rel = (n.startBeat - loopStart) / static_cast<float>(tiles);
            n.startBeat = tileOffset + rel;
            n.lengthBeats = n.lengthBeats / static_cast<float>(tiles);
            outNotes.push_back(n);
            outSpans.push_back(spans[i]);
        }
    }
    notes = std::move(outNotes);
    spans = std::move(outSpans);
}

} // namespace

CompileResult Compile(const std::string& patternText, float loopStartBeat, float loopEndBeat,
                       int cycleIndex, uint32_t seed) {
    CompileResult result;
    if (loopEndBeat <= loopStartBeat) {
        result.ok = false;
        result.error = "loop range must be positive (loopEndBeat > loopStartBeat)";
        return result;
    }

    // live-PLAN L5: split off pipe-separated transform stages first. Stage 0
    // is the mini-notation body (which may still carry L2's legacy trailing
    // " scale=<name>" form); stages 1.. are parsed as transforms/scale=.
    std::vector<std::string> pipeStages = SplitPipeStages(patternText);
    std::string pipeScaleOverride;
    std::vector<ParsedTransform> transforms;
    for (size_t i = 1; i < pipeStages.size(); ++i) {
        std::string stageError;
        if (!ParseTransformStage(pipeStages[i], pipeScaleOverride, transforms, stageError)) {
            result.ok = false;
            result.error = stageError;
            return result;
        }
    }

    auto [body, trailingScaleName] = SplitTrailingScale(pipeStages[0]);
    std::string scaleName = !pipeScaleOverride.empty() ? pipeScaleOverride : trailingScaleName;

    Parser parser(body);
    PatternNode root = parser.ParseTop();
    if (parser.failed()) {
        result.ok = false;
        result.error = parser.errorMsg();
        result.errorColumn = parser.errorPos();
        return result;
    }

    FlattenCtx ctx;
    ctx.cycleIndex = cycleIndex;
    uint32_t rngSeed = seed != 0 ? seed : FnvHash(patternText);
    ctx.rngState = rngSeed != 0 ? rngSeed : 1u; // xorshift state must stay non-zero
    ctx.scaleName = std::move(scaleName);
    ctx.notes = &result.notes;
    ctx.spans = &result.spans;

    FlattenNode(root, loopStartBeat, loopEndBeat - loopStartBeat, ctx);

    // Stable, scan-friendly ordering (matches hand-authored NOTE lines,
    // which are naturally written start-beat-ascending).
    std::vector<size_t> order(result.notes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return result.notes[a].startBeat < result.notes[b].startBeat;
    });
    std::vector<Note> sortedNotes;
    std::vector<StepSpan> sortedSpans;
    sortedNotes.reserve(order.size());
    sortedSpans.reserve(order.size());
    for (size_t idx : order) {
        sortedNotes.push_back(result.notes[idx]);
        sortedSpans.push_back(result.spans[idx]);
    }
    result.notes = std::move(sortedNotes);
    result.spans = std::move(sortedSpans);

    // live-PLAN L5: apply pipe transforms, left to right, over the
    // flattened+sorted cycle.
    for (const auto& tr : transforms) {
        switch (tr.kind) {
            case ParsedTransform::Kind::Rev:
                ApplyRev(result.notes, result.spans, loopStartBeat, loopEndBeat);
                break;
            case ParsedTransform::Kind::Fast:
                ApplyFast(result.notes, result.spans, loopStartBeat, loopEndBeat, tr.factor);
                break;
            case ParsedTransform::Kind::EveryRev:
                if (tr.everyN > 0 && (cycleIndex % tr.everyN) == 0) {
                    ApplyRev(result.notes, result.spans, loopStartBeat, loopEndBeat);
                }
                break;
        }
    }

    return result;
}

} // namespace PatternCompiler
