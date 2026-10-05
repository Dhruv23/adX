#include "engine/format/adx/Parser.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "engine/format/adx/NoteName.h"
#include "engine/format/adx/V1Shim.h"
#include "engine/format/adx/Value.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"
#include "engine/project/TypeCatalog.h"
#include "engine/project/Validate.h"
#include "engine/project/commands/AutomationCommands.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/MixerCommands.h"
#include "engine/project/commands/NoteCommands.h"
#include "engine/project/commands/PatternCommands.h"
#include "engine/project/commands/PlaylistCommands.h"
#include "engine/project/commands/ProjectCommands.h"

namespace adx::format {
namespace {

using project::Project;

/// One line, plus the lines indented under it.
struct BlockNode {
    std::size_t line{0};
    std::vector<BlockNode> children;
};

/// Builds the indentation tree for one section.
///
/// Blank and comment lines take part in no nesting decision (they are skipped
/// here), which is what lets a comment sit between two notes without ending the
/// block they are in.
[[nodiscard]] std::vector<BlockNode> buildBlocks(std::span<const Line> lines,
                                                 const Section& section) {
    std::vector<BlockNode> roots;
    // (indent, path from roots). The path is rebuilt on every push because a
    // std::vector of children invalidates pointers when it grows.
    std::vector<std::pair<std::uint32_t, std::vector<std::size_t>>> stack;

    for (const std::size_t index : section.lineIndices) {
        const Line& line = lines[index];
        if (line.kind == Line::Kind::Blank || line.kind == Line::Kind::Comment) {
            continue;
        }
        while (!stack.empty() && stack.back().first >= line.indent) {
            stack.pop_back();
        }

        std::vector<BlockNode>* target = &roots;
        for (const std::size_t step :
             stack.empty() ? std::vector<std::size_t>{} : stack.back().second) {
            target = &(*target)[step].children;
        }
        target->push_back(BlockNode{.line = index, .children = {}});

        std::vector<std::size_t> path =
            stack.empty() ? std::vector<std::size_t>{} : stack.back().second;
        path.push_back(target->size() - 1);
        stack.emplace_back(line.indent, std::move(path));
    }
    return roots;
}

[[nodiscard]] std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - ('a' - 'A'));
        }
    }
    return out;
}

/// The key=value lines directly under a section or a block, matched
/// case-insensitively. A key that appears twice keeps its last value, per
/// docs/adx-format-v2.md §4.2.
class KeyTable {
public:
    KeyTable(std::span<Line> lines, std::span<const BlockNode> nodes, DiagnosticList& diagnostics) {
        for (const BlockNode& node : nodes) {
            const Line& line = lines[node.line];
            if (line.kind != Line::Kind::KeyValue) {
                continue;
            }
            const std::string key = upper(line.key);
            const auto existing = m_entries.find(key);
            if (existing != m_entries.end()) {
                diagnostics.add(code::kDuplicateKey, lines[existing->second].keySpan,
                                "key '" + line.key + "' appears more than once; the last wins");
                // Claimed so the superseded line does not also come back as residue,
                // which would re-emit a value the model deliberately discarded.
                lines[existing->second].claimed = true;
            }
            m_entries[key] = node.line;
        }
    }

    /// The line for `key`, claimed, or nullptr.
    [[nodiscard]] Line* take(std::span<Line> lines, std::string_view key) {
        const auto match = m_entries.find(upper(key));
        if (match == m_entries.end()) {
            return nullptr;
        }
        Line& line = lines[match->second];
        line.claimed = true;
        m_entries.erase(match);
        return &line;
    }

private:
    std::unordered_map<std::string, std::size_t> m_entries;
};

class V2Parser {
public:
    V2Parser(Document& document, Project& project, project::CommandStack& stack,
             DiagnosticList& diagnostics, const ParseOptions& options)
        : m_doc(document), m_project(project), m_stack(stack), m_diag(diagnostics),
          m_options(options) {}

    void run();

private:
    struct ChannelBinding {
        std::size_t section{0};
        core::ChannelId id;
        /// OUTPUT=, claimed while reading the channel but resolved afterwards:
        /// inserts may be declared after channels, and a file's section order is its
        /// author's business.
        bool hasOutput{false};
        std::string outputText;
        Span outputSpan;
    };
    struct PatternBinding {
        std::size_t section{0};
        core::PatternId id;
    };

    [[nodiscard]] std::span<Line> lines() noexcept {
        return m_doc.lines();
    }
    void execute(std::unique_ptr<project::Command> command) {
        m_stack.execute(std::move(command), m_project);
    }

    void parseMeter(Section& section);
    void parseTempo(Section& section);
    void parseProject(Section& section);
    void parseMixer(Section& section);
    void parseInsertChildren(core::InsertId insert, const BlockNode& node);
    [[nodiscard]] ChannelBinding parseChannelHeader(Section& section, std::size_t index);
    void parseChannelLines(core::ChannelId id, std::span<const BlockNode> nodes);
    void parseChannelOutput(const ChannelBinding& binding);
    core::PatternId parsePatternHeader(Section& section);
    void parsePatternBody(const PatternBinding& binding);
    void parseNotesBlock(core::PatternId pattern, const BlockNode& node);
    void parseAutomationBlock(core::PatternId pattern, const BlockNode& node);
    std::vector<project::Breakpoint> parseBreakpoints(const BlockNode& node);
    void parsePlaylist(Section& section);
    void parsePlaylistItem(core::PlaylistTrackId track, const BlockNode& node);
    /// ADX1004 for a name `type` does not declare, ADX2001 for a value outside the
    /// declared range (P2-4). Both keep the value. With no known type, the name is
    /// checked against the generic table instead.
    void checkParam(const project::TypeInfo* type, const Token& token, double value);
    void parseClipEnvelope(const BlockNode& node, project::PlaylistItem& item);
    void parseMarkers(Section& section);

    [[nodiscard]] bool resolveInsert(std::string_view text, Span span, core::InsertId& out);
    [[nodiscard]] bool resolveChannel(std::string_view text, Span span, core::ChannelId& out);
    void unknownKeys(KeyTable& table, std::span<const BlockNode> nodes);
    void reportUnknownSection(const Section& section);

    Document& m_doc;
    Project& m_project;
    project::CommandStack& m_stack;
    DiagnosticList& m_diag;
    const ParseOptions& m_options;

    std::vector<ChannelBinding> m_channels;
    std::vector<PatternBinding> m_patterns;
};

// --- helpers -----------------------------------------------------------------

void V2Parser::unknownKeys(KeyTable& table, std::span<const BlockNode> nodes) {
    for (const BlockNode& node : nodes) {
        const Line& line = lines()[node.line];
        if (line.claimed) {
            continue;
        }
        if (line.kind == Line::Kind::KeyValue) {
            m_diag.add(code::kUnknownKey, line.keySpan,
                       "unknown key '" + line.key + "'; preserved on save");
        } else {
            m_diag.add(code::kUnknownKey, line.contentSpan(),
                       "unrecognised line; preserved on save");
        }
    }
    (void)table;
}

void V2Parser::reportUnknownSection(const Section& section) {
    m_diag.add(code::kUnknownSection, section.span,
               "unknown section '" + section.type + "'; preserved on save");
}

bool V2Parser::resolveInsert(std::string_view text, Span span, core::InsertId& out) {
    if (text == "master") {
        out = m_project.mixer.master;
        return out.valid();
    }
    constexpr std::string_view kPrefix = "insert.";
    if (!text.starts_with(kPrefix)) {
        m_diag.add(code::kMalformedReference, span,
                   "expected insert.N or master, got '" + std::string(text) + "'");
        return false;
    }
    const std::string_view digits = text.substr(kPrefix.size());
    std::uint32_t value = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() || value == 0) {
        m_diag.add(code::kMalformedReference, span,
                   "'" + std::string(text) + "' is not an insert reference");
        return false;
    }
    const core::InsertId id{value};
    if (m_project.mixer.find(id) == nullptr) {
        m_diag.add(code::kUnknownInsertRef, span,
                   "insert " + std::to_string(value) + " does not exist");
        return false;
    }
    out = id;
    return true;
}

bool V2Parser::resolveChannel(std::string_view text, Span span, core::ChannelId& out) {
    const project::Channel* channel = m_project.findChannelByName(text);
    if (channel == nullptr) {
        m_diag.add(code::kUnknownChannelRef, span, "no channel named '" + std::string(text) + "'");
        return false;
    }
    out = channel->id;
    return true;
}

// --- sections ----------------------------------------------------------------

void V2Parser::parseMeter(Section& section) {
    section.claimed = true;
    for (const BlockNode& node : buildBlocks(lines(), section)) {
        Line& line = lines()[node.line];
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.size() != 2) {
            m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                       "a meter line is a position and a time signature");
            continue;
        }
        core::Ticks at;
        // Each event resolves against the map built so far, which is what makes a
        // meter change written in bars work: `16:0:0 7/8` means bar 16 counted in
        // whatever meter was in force up to there.
        if (!parsePosition(tokens[0].text, m_project.tempo, tokens[0].span, m_diag, at)) {
            continue;
        }
        const std::size_t slash = tokens[1].text.find('/');
        if (slash == std::string::npos) {
            m_diag.add(code::kMalformedNumber, tokens[1].span, "expected numerator/denominator");
            continue;
        }
        std::int64_t numerator = 0;
        std::int64_t denominator = 0;
        if (!parseInt64(std::string_view(tokens[1].text).substr(0, slash), tokens[1].span, m_diag,
                        numerator) ||
            !parseInt64(std::string_view(tokens[1].text).substr(slash + 1), tokens[1].span, m_diag,
                        denominator)) {
            continue;
        }
        if (numerator <= 0 || numerator > 64) {
            m_diag.add(code::kValueOutOfRange, tokens[1].span, "meter numerator must be 1..64");
            numerator = std::clamp<std::int64_t>(numerator, 1, 64);
        }
        if (denominator != 1 && denominator != 2 && denominator != 4 && denominator != 8 &&
            denominator != 16 && denominator != 32 && denominator != 64) {
            m_diag.add(code::kBadMeterDenominator, tokens[1].span,
                       "denominator " + std::to_string(denominator) +
                           " does not divide a whole note exactly; using 4");
            denominator = 4;
        }
        execute(std::make_unique<project::SetMeterEvent>(at, static_cast<std::uint16_t>(numerator),
                                                         static_cast<std::uint16_t>(denominator)));
        line.claimed = true;
    }
}

void V2Parser::parseTempo(Section& section) {
    section.claimed = true;
    for (const BlockNode& node : buildBlocks(lines(), section)) {
        Line& line = lines()[node.line];
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.size() < 2 || tokens.size() > 3) {
            m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                       "a tempo line is a position, a bpm, and optionally 'ramp'");
            continue;
        }
        core::Ticks at;
        double bpm = 0.0;
        if (!parsePosition(tokens[0].text, m_project.tempo, tokens[0].span, m_diag, at) ||
            !parseDouble(tokens[1].text, tokens[1].span, m_diag, bpm)) {
            continue;
        }
        if (bpm < core::kMinBpm || bpm > core::kMaxBpm) {
            m_diag.add(code::kTempoOutOfRange, tokens[1].span,
                       "tempo " + formatDouble(bpm) + " is outside 1..999; clamped");
        }
        bool ramp = false;
        if (tokens.size() == 3) {
            if (tokens[2].text != "ramp") {
                m_diag.add(code::kUnknownInlineKey, tokens[2].span,
                           "expected 'ramp', got '" + tokens[2].text + "'");
            } else {
                ramp = true;
            }
        }
        execute(std::make_unique<project::SetTempoEvent>(at, bpm, ramp));
        line.claimed = true;
    }
}

void V2Parser::parseProject(Section& section) {
    section.claimed = true;
    const std::vector<BlockNode> nodes = buildBlocks(lines(), section);
    KeyTable keys(lines(), nodes, m_diag);

    project::ProjectMeta meta = m_project.meta;
    if (Line* line = keys.take(lines(), "ADX_VERSION")) {
        std::int64_t version = project::kAdxVersion;
        if (parseInt64(line->value, line->valueSpan, m_diag, version)) {
            meta.version = static_cast<int>(version);
            if (version > project::kAdxVersion) {
                m_diag.add(code::kFutureVersion, line->valueSpan,
                           "this file is version " + std::to_string(version) +
                               "; anything newer than " + std::to_string(project::kAdxVersion) +
                               " is preserved but not understood");
            }
        }
    }
    if (Line* line = keys.take(lines(), "TITLE")) {
        meta.title = decodeValue(line->value);
    }
    if (Line* line = keys.take(lines(), "AUTHOR")) {
        meta.author = decodeValue(line->value);
    }
    if (Line* line = keys.take(lines(), "CREATED")) {
        meta.created = decodeValue(line->value);
    }
    if (Line* line = keys.take(lines(), "TUNING")) {
        double tuning = 440.0;
        if (parseDouble(line->value, line->valueSpan, m_diag, tuning)) {
            meta.tuning = tuning;
        }
    }
    if (Line* line = keys.take(lines(), "LOOP")) {
        const std::size_t dash = line->value.find('-', 1);
        if (dash == std::string::npos) {
            m_diag.add(code::kMalformedPosition, line->valueSpan, "expected start-end");
        } else {
            core::Ticks start;
            core::Ticks end;
            const std::string_view text = line->value;
            if (parsePosition(text.substr(0, dash), m_project.tempo, line->valueSpan, m_diag,
                              start) &&
                parsePosition(text.substr(dash + 1), m_project.tempo, line->valueSpan, m_diag,
                              end)) {
                meta.loopEnabled = true;
                meta.loopStart = start;
                meta.loopEnd = end;
            }
        }
    }

    execute(std::make_unique<project::SetMeta>(std::move(meta)));
    unknownKeys(keys, nodes);
}

void V2Parser::parseMarkers(Section& section) {
    section.claimed = true;
    for (const BlockNode& node : buildBlocks(lines(), section)) {
        Line& line = lines()[node.line];
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.size() != 2) {
            m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                       "a marker line is a position and a name");
            continue;
        }
        core::Ticks at;
        if (!parsePosition(tokens[0].text, m_project.tempo, tokens[0].span, m_diag, at)) {
            continue;
        }
        execute(std::make_unique<project::AddMarker>(at, tokens[1].text));
        line.claimed = true;
    }
}

// --- mixer -------------------------------------------------------------------

void V2Parser::parseMixer(Section& section) {
    section.claimed = true;
    const std::vector<BlockNode> nodes = buildBlocks(lines(), section);

    // Three sweeps, because each depends on the one before it: every insert has to
    // exist before a send can name one, and every send has to exist before a route
    // can be checked against the same graph.
    std::vector<std::pair<const BlockNode*, core::InsertId>> created;
    for (const BlockNode& node : nodes) {
        Line& line = lines()[node.line];
        if (line.kind != Line::Kind::Positional) {
            continue;
        }
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.empty() || tokens.front().text != "INSERT") {
            continue;
        }
        if (tokens.size() < 2) {
            m_diag.add(code::kWrongFieldCount, line.contentSpan(), "INSERT needs an id");
            continue;
        }
        std::int64_t id = 0;
        if (!parseInt64(tokens[1].text, tokens[1].span, m_diag, id) || id <= 0) {
            m_diag.add(code::kZeroId, tokens[1].span, "an insert id must be a positive integer");
            continue;
        }

        project::Insert insert;
        insert.id = core::InsertId{static_cast<std::uint32_t>(id)};
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            const Token& token = tokens[i];
            if (!token.isKeyValue()) {
                m_diag.add(code::kWrongFieldCount, token.span, "expected key=value");
                continue;
            }
            double number = 0.0;
            bool flag = false;
            if (token.text == "name") {
                insert.name = token.value;
            } else if (token.text == "gain" &&
                       parseDouble(token.value, token.valueSpan, m_diag, number)) {
                insert.gain = static_cast<float>(number);
            } else if (token.text == "pan" &&
                       parseDouble(token.value, token.valueSpan, m_diag, number)) {
                insert.pan = static_cast<float>(number);
            } else if (token.text == "width" &&
                       parseDouble(token.value, token.valueSpan, m_diag, number)) {
                insert.stereoSeparation = static_cast<float>(number);
            } else if (token.text == "mute" &&
                       parseBool(token.value, token.valueSpan, m_diag, flag)) {
                insert.muted = flag;
            } else if (token.text == "solo" &&
                       parseBool(token.value, token.valueSpan, m_diag, flag)) {
                insert.soloed = flag;
            } else if (token.text == "invert" &&
                       parseBool(token.value, token.valueSpan, m_diag, flag)) {
                insert.polarityInvert = flag;
            } else if (token.text == "color") {
                project::Color color;
                if (parseColor(token.value, token.valueSpan, m_diag, color)) {
                    insert.color = color;
                }
            } else if (token.text != "gain" && token.text != "pan" && token.text != "width" &&
                       token.text != "mute" && token.text != "solo" && token.text != "invert") {
                m_diag.add(code::kUnknownInlineKey, token.span,
                           "unknown key '" + token.text + "' on an INSERT line");
            }
        }

        auto command = std::make_unique<project::AddInsert>(std::move(insert));
        const project::AddInsert* raw = command.get();
        execute(std::move(command));
        created.emplace_back(&node, raw->created());
        line.claimed = true;
    }

    for (const auto& [node, id] : created) {
        parseInsertChildren(id, *node);
    }

    for (const BlockNode& node : nodes) {
        Line& line = lines()[node.line];
        if (line.claimed || line.kind != Line::Kind::Positional) {
            continue;
        }
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.empty() || tokens.front().text != "ROUTE") {
            continue;
        }
        if (tokens.size() != 4 || tokens[2].text != "->") {
            m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                       "a route is 'ROUTE insert.a -> insert.b'");
            continue;
        }
        core::InsertId from;
        core::InsertId to;
        if (!resolveInsert(tokens[1].text, tokens[1].span, from) ||
            !resolveInsert(tokens[3].text, tokens[3].span, to)) {
            continue;
        }
        execute(std::make_unique<project::AddRoute>(from, to));
        line.claimed = true;
    }

    for (const BlockNode& node : nodes) {
        const Line& line = lines()[node.line];
        if (!line.claimed) {
            m_diag.add(code::kUnknownKey, line.contentSpan(),
                       "unrecognised line in [MIXER]; preserved on save");
        }
    }
}

void V2Parser::parseInsertChildren(core::InsertId insert, const BlockNode& node) {
    for (const BlockNode& child : node.children) {
        Line& line = lines()[child.line];
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.empty()) {
            continue;
        }

        if (tokens.front().text == "SLOT") {
            if (tokens.size() < 3) {
                m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                           "a slot is 'SLOT <id> <Type> [key=value...]'");
                continue;
            }
            std::int64_t id = 0;
            if (!parseInt64(tokens[1].text, tokens[1].span, m_diag, id) || id <= 0) {
                m_diag.add(code::kZeroId, tokens[1].span, "a slot id must be a positive integer");
                continue;
            }
            project::Slot slot;
            slot.id = core::SlotId{static_cast<std::uint32_t>(id)};
            slot.type = tokens[2].text;
            for (std::size_t i = 3; i < tokens.size(); ++i) {
                const Token& token = tokens[i];
                if (!token.isKeyValue()) {
                    m_diag.add(code::kWrongFieldCount, token.span, "expected key=value");
                    continue;
                }
                if (token.text == "bypass") {
                    bool flag = false;
                    if (parseBool(token.value, token.valueSpan, m_diag, flag)) {
                        slot.bypass = flag;
                    }
                    continue;
                }
                if (token.text == "sidechain") {
                    core::InsertId key;
                    if (resolveInsert(token.value, token.valueSpan, key)) {
                        slot.sidechain = key;
                    }
                    continue;
                }
                if (token.text == "ir") {
                    // One pool entry per file, shared with zones and AUDIO items.
                    if (const project::SampleRef* existing =
                            m_project.resources.findByPath(token.value)) {
                        slot.impulse = existing->id;
                    } else {
                        auto sample = std::make_unique<project::AddSample>(token.value);
                        const project::AddSample* raw = sample.get();
                        execute(std::move(sample));
                        slot.impulse = raw->created();
                    }
                    continue;
                }
                double number = 0.0;
                if (!parseDouble(token.value, token.valueSpan, m_diag, number)) {
                    continue;
                }
                if (token.text == "mix") {
                    slot.mix = static_cast<float>(number);
                } else {
                    // An unknown one is kept, not dropped (FINAL_PLAN §6 rule 4).
                    checkParam(project::findEffectType(slot.type), token, number);
                    slot.params.push_back(project::SlotParam{.name = token.text, .value = number});
                }
            }
            execute(std::make_unique<project::AddSlot>(insert, std::move(slot)));
            line.claimed = true;
            continue;
        }

        if (tokens.front().text == "SEND") {
            if (tokens.size() < 3) {
                m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                           "a send is 'SEND <id> insert.N [level=..] [pre=..]'");
                continue;
            }
            std::int64_t id = 0;
            if (!parseInt64(tokens[1].text, tokens[1].span, m_diag, id) || id <= 0) {
                m_diag.add(code::kZeroId, tokens[1].span, "a send id must be a positive integer");
                continue;
            }
            core::InsertId target;
            if (!resolveInsert(tokens[2].text, tokens[2].span, target)) {
                continue;
            }
            float level = 0.0F;
            bool preFader = false;
            for (std::size_t i = 3; i < tokens.size(); ++i) {
                const Token& token = tokens[i];
                if (!token.isKeyValue()) {
                    m_diag.add(code::kWrongFieldCount, token.span, "expected key=value");
                    continue;
                }
                if (token.text == "level") {
                    double number = 0.0;
                    if (parseDouble(token.value, token.valueSpan, m_diag, number)) {
                        level = static_cast<float>(number);
                    }
                } else if (token.text == "pre") {
                    bool flag = false;
                    if (parseBool(token.value, token.valueSpan, m_diag, flag)) {
                        preFader = flag;
                    }
                } else {
                    m_diag.add(code::kUnknownInlineKey, token.span,
                               "unknown key '" + token.text + "' on a SEND line");
                }
            }
            execute(std::make_unique<project::AddSend>(
                insert, target, level, preFader, core::SendId{static_cast<std::uint32_t>(id)}));
            line.claimed = true;
            continue;
        }

        m_diag.add(code::kUnknownKey, line.contentSpan(),
                   "unrecognised line inside an insert; preserved on save");
    }
}

// --- channels ----------------------------------------------------------------

V2Parser::ChannelBinding V2Parser::parseChannelHeader(Section& section, std::size_t index) {
    section.claimed = true;
    ChannelBinding binding;
    binding.section = index;
    if (!project::isValidEntityName(section.name)) {
        m_diag.add(code::kBadNameCharacter, section.span,
                   "'" + section.name + "' is not a usable channel name");
        return binding;
    }
    if (m_project.findChannelByName(section.name) != nullptr) {
        m_diag.add(code::kDuplicateName, section.span,
                   "a channel named '" + section.name + "' already exists");
        return binding;
    }

    const std::vector<BlockNode> nodes = buildBlocks(lines(), section);
    KeyTable keys(lines(), nodes, m_diag);

    project::Channel channel;
    channel.name = section.name;

    if (Line* line = keys.take(lines(), "INSTRUMENT")) {
        channel.instrument.type = decodeValue(line->value);
    }
    if (Line* line = keys.take(lines(), "VOICEBANK")) {
        channel.instrument.voicebank = decodeValue(line->value);
    }
    if (Line* line = keys.take(lines(), "POLYPHONY")) {
        std::int64_t value = channel.maxPolyphony;
        if (parseInt64(line->value, line->valueSpan, m_diag, value)) {
            if (value < 1 || value > 256) {
                m_diag.add(code::kValueOutOfRange, line->valueSpan,
                           "polyphony must be 1..256; clamped");
            }
            channel.maxPolyphony =
                static_cast<std::uint16_t>(std::clamp<std::int64_t>(value, 1, 256));
        }
    }
    if (Line* line = keys.take(lines(), "STEAL")) {
        if (!project::voiceStealModeFromString(line->value, channel.stealMode)) {
            m_diag.add(code::kUnknownInlineKey, line->valueSpan,
                       "unknown voice-stealing mode '" + line->value + "'",
                       "one of oldest-released, oldest, quietest, none");
        }
    }
    const auto number = [&](std::string_view key, float& target) {
        if (Line* line = keys.take(lines(), key)) {
            double value = 0.0;
            if (parseDouble(line->value, line->valueSpan, m_diag, value)) {
                target = static_cast<float>(value);
            }
        }
    };
    number("VOLUME", channel.volume);
    number("PAN", channel.pan);
    number("PITCH", channel.pitchOffsetCents);
    const auto flag = [&](std::string_view key, bool& target) {
        if (Line* line = keys.take(lines(), key)) {
            bool value = false;
            if (parseBool(line->value, line->valueSpan, m_diag, value)) {
                target = value;
            }
        }
    };
    flag("MUTE", channel.muted);
    flag("SOLO", channel.soloed);
    if (Line* line = keys.take(lines(), "COLOR")) {
        project::Color color;
        if (parseColor(line->value, line->valueSpan, m_diag, color)) {
            channel.color = color;
        }
    }
    if (Line* line = keys.take(lines(), "OUTPUT")) {
        binding.hasOutput = true;
        binding.outputText = line->value;
        binding.outputSpan = line->valueSpan;
    }

    auto command = std::make_unique<project::AddChannel>(std::move(channel));
    const project::AddChannel* raw = command.get();
    execute(std::move(command));
    const core::ChannelId id = raw->created();
    binding.id = id;

    parseChannelLines(id, nodes);

    unknownKeys(keys, nodes);
    return binding;
}

void V2Parser::parseChannelLines(core::ChannelId id, std::span<const BlockNode> nodes) {
    std::vector<project::SampleZone> zones;
    bool hasZones = false;
    for (const BlockNode& node : nodes) {
        Line& line = lines()[node.line];
        if (line.claimed || line.kind != Line::Kind::Positional) {
            continue;
        }
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.empty()) {
            continue;
        }
        if (tokens.front().text == "PARAM") {
            if (tokens.size() < 2 || !tokens[1].isKeyValue()) {
                m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                           "a parameter is 'PARAM name=value [curve=...]'");
                continue;
            }
            double value = 0.0;
            if (!parseDouble(tokens[1].value, tokens[1].valueSpan, m_diag, value)) {
                continue;
            }
            if (const project::Channel* channel = m_project.find(id)) {
                checkParam(project::findInstrumentType(channel->instrument.type), tokens[1], value);
            }
            bool hasCurve = false;
            core::Curve curve;
            for (std::size_t i = 2; i < tokens.size(); ++i) {
                if (tokens[i].isKeyValue() && tokens[i].text == "curve") {
                    hasCurve = parseCurve(tokens[i].value, tokens[i].valueSpan, m_diag, curve);
                } else {
                    m_diag.add(code::kUnknownInlineKey, tokens[i].span,
                               "unknown key on a PARAM line");
                }
            }
            if (hasCurve) {
                execute(
                    std::make_unique<project::SetChannelParam>(id, tokens[1].text, value, curve));
            } else {
                execute(std::make_unique<project::SetChannelParam>(id, tokens[1].text, value));
            }
            line.claimed = true;
            continue;
        }
        if (tokens.front().text == "ARP") {
            project::ArpSettings arp;
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                const Token& token = tokens[i];
                if (!token.isKeyValue()) {
                    m_diag.add(code::kWrongFieldCount, token.span, "expected key=value");
                    continue;
                }
                if (token.text == "mode") {
                    if (!project::arpModeFromString(token.value, arp.mode)) {
                        m_diag.add(code::kUnknownInlineKey, token.valueSpan,
                                   "unknown arp mode '" + token.value + "'");
                    }
                } else if (token.text == "rate") {
                    (void)parseFraction(token.value, token.valueSpan, m_diag, arp.rate);
                } else if (token.text == "octaves") {
                    std::int64_t value = 1;
                    if (parseInt64(token.value, token.valueSpan, m_diag, value)) {
                        arp.octaves =
                            static_cast<std::uint16_t>(std::clamp<std::int64_t>(value, 1, 8));
                    }
                } else if (token.text == "gate") {
                    double value = 0.0;
                    if (parseDouble(token.value, token.valueSpan, m_diag, value)) {
                        arp.gate = static_cast<float>(value);
                    }
                } else {
                    m_diag.add(code::kUnknownInlineKey, token.span,
                               "unknown key '" + token.text + "' on an ARP line");
                }
            }
            execute(std::make_unique<project::SetChannelArp>(id, arp));
            line.claimed = true;
            continue;
        }
        if (tokens.front().text == "ZONE") {
            if (tokens.size() < 2 || tokens[1].isKeyValue()) {
                m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                           "a zone is 'ZONE \"sample.wav\" key=... root=...'");
                continue;
            }
            // The same file named by ten zones is one pool entry (Resources::findByPath),
            // exactly as AUDIO items share theirs.
            project::SampleZone zone;
            if (const project::SampleRef* existing =
                    m_project.resources.findByPath(tokens[1].text)) {
                zone.sample = existing->id;
            } else {
                auto sample = std::make_unique<project::AddSample>(tokens[1].text);
                const project::AddSample* raw = sample.get();
                execute(std::move(sample));
                zone.sample = raw->created();
            }
            parseZoneFields(std::span<const Token>{tokens}.subspan(2), m_diag, zone);
            zones.push_back(zone);
            hasZones = true;
            line.claimed = true;
            continue;
        }
    }
    if (hasZones) {
        execute(std::make_unique<project::SetChannelZones>(id, std::move(zones)));
    }
}

void V2Parser::parseChannelOutput(const ChannelBinding& binding) {
    core::InsertId output = m_project.mixer.master;
    if (binding.hasOutput) {
        core::InsertId resolved;
        if (resolveInsert(binding.outputText, binding.outputSpan, resolved)) {
            output = resolved;
        }
    }
    execute(std::make_unique<project::SetChannelOutput>(binding.id, output));
}

// --- patterns ----------------------------------------------------------------

core::PatternId V2Parser::parsePatternHeader(Section& section) {
    section.claimed = true;
    if (!project::isValidEntityName(section.name)) {
        m_diag.add(code::kBadNameCharacter, section.span,
                   "'" + section.name + "' is not a usable pattern name");
        return {};
    }
    if (m_project.findPatternByName(section.name) != nullptr) {
        m_diag.add(code::kDuplicateName, section.span,
                   "a pattern named '" + section.name + "' already exists");
        return {};
    }

    const std::vector<BlockNode> nodes = buildBlocks(lines(), section);
    KeyTable keys(lines(), nodes, m_diag);

    project::Pattern pattern;
    pattern.name = section.name;
    if (Line* line = keys.take(lines(), "LENGTH")) {
        core::Ticks length;
        if (parseDuration(line->value, m_project.tempo, line->valueSpan, m_diag, length)) {
            pattern.length = length;
        }
    }
    if (Line* line = keys.take(lines(), "COLOR")) {
        project::Color color;
        if (parseColor(line->value, line->valueSpan, m_diag, color)) {
            pattern.color = color;
        }
    }

    auto command = std::make_unique<project::AddPattern>(std::move(pattern));
    const project::AddPattern* raw = command.get();
    execute(std::move(command));
    return raw->created();
}

void V2Parser::parsePatternBody(const PatternBinding& binding) {
    const Section& section = m_doc.sections()[binding.section];
    const std::vector<BlockNode> nodes = buildBlocks(lines(), section);

    for (const BlockNode& node : nodes) {
        Line& line = lines()[node.line];
        if (line.claimed || line.kind != Line::Kind::Positional) {
            continue;
        }
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.empty()) {
            continue;
        }
        if (tokens.front().text == "NOTES") {
            parseNotesBlock(binding.id, node);
            continue;
        }
        if (tokens.front().text == "MINI") {
            if (tokens.size() != 2) {
                m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                           "MINI names exactly one channel");
                continue;
            }
            core::ChannelId channel;
            if (!resolveChannel(tokens[1].text, tokens[1].span, channel)) {
                continue;
            }
            std::vector<std::string> body;
            for (const BlockNode& child : node.children) {
                body.push_back(lines()[child.line].content);
                lines()[child.line].claimed = true;
            }
            if (body.empty()) {
                m_diag.add(code::kEmptyBlock, line.contentSpan(),
                           "MINI has no indented body; mini-notation must be indented under it");
            }
            execute(
                std::make_unique<project::SetMiniNotation>(binding.id, channel, std::move(body)));
            line.claimed = true;
            continue;
        }
        if (tokens.front().text == "AUTOMATION") {
            parseAutomationBlock(binding.id, node);
            continue;
        }
    }

    for (const BlockNode& node : nodes) {
        const Line& line = lines()[node.line];
        if (line.claimed) {
            continue;
        }
        m_diag.add(code::kUnknownKey, line.contentSpan(),
                   "unrecognised line in a pattern; preserved on save");
    }
}

void V2Parser::parseNotesBlock(core::PatternId pattern, const BlockNode& node) {
    Line& header = lines()[node.line];
    const std::vector<Token> tokens = tokenize(header, m_diag);
    if (tokens.size() != 2) {
        m_diag.add(code::kWrongFieldCount, header.contentSpan(), "NOTES names exactly one channel");
        return;
    }
    core::ChannelId channel;
    if (!resolveChannel(tokens[1].text, tokens[1].span, channel)) {
        return;
    }
    if (node.children.empty()) {
        m_diag.add(code::kEmptyBlock, header.contentSpan(),
                   "NOTES has no indented body; notes must be indented under it");
    }

    const project::Pattern* owner = m_project.find(pattern);
    const core::Ticks patternLength = owner != nullptr ? owner->length : core::Ticks{0};

    std::vector<project::Note> notes;
    std::vector<project::NoteExtrasAt> extras;
    notes.reserve(node.children.size());
    for (const BlockNode& child : node.children) {
        Line& line = lines()[child.line];
        const std::vector<Token> fields = tokenize(line, m_diag);
        if (fields.size() < 4) {
            m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                       "a note is 'name start length velocity [key=value...]'");
            continue;
        }
        project::Note note;
        std::uint32_t badOffset = 0;
        if (!noteNameToMidi(fields[0].text, note.pitch, badOffset)) {
            m_diag.add(code::kBadNoteName, subSpan(fields[0].span, badOffset, 1),
                       "'" + fields[0].text + "' is not a note name");
            continue;
        }
        std::int64_t velocity = project::kDefaultVelocity;
        if (!parsePosition(fields[1].text, m_project.tempo, fields[1].span, m_diag, note.start) ||
            !parseDuration(fields[2].text, m_project.tempo, fields[2].span, m_diag, note.length) ||
            !parseInt64(fields[3].text, fields[3].span, m_diag, velocity)) {
            continue;
        }
        if (velocity < 0 || velocity > 127) {
            m_diag.add(code::kValueOutOfRange, fields[3].span, "velocity must be 0..127; clamped");
        }
        note.velocity = static_cast<std::uint8_t>(std::clamp<std::int64_t>(velocity, 0, 127));
        if (patternLength.value > 0 && note.start >= patternLength) {
            m_diag.add(code::kNoteOutsidePattern, fields[1].span,
                       "this note starts past the end of the pattern; kept");
        }

        project::NoteExtras extra;
        for (std::size_t i = 4; i < fields.size(); ++i) {
            const Token& token = fields[i];
            if (!token.isKeyValue()) {
                m_diag.add(code::kWrongFieldCount, token.span, "expected key=value");
                continue;
            }
            // The three extensions are not numbers, so they are taken before the
            // numeric keys below (phase_4.md §4.0).
            if (token.text == "slide") {
                project::NoteSlide slide;
                if (parseSlide(token.value, m_project.tempo, token.valueSpan, m_diag, slide)) {
                    extra.slide = slide;
                }
                continue;
            }
            if (token.text == "bend") {
                (void)parseBend(token.value, m_project.tempo, token.valueSpan, m_diag,
                                extra.pitchCurve);
                continue;
            }
            if (token.text == "lyric") {
                extra.lyric = token.value;
                continue;
            }
            double value = 0.0;
            if (!parseDouble(token.value, token.valueSpan, m_diag, value)) {
                continue;
            }
            if (token.text == "pan") {
                note.pan = static_cast<float>(value);
            } else if (token.text == "cutoff") {
                note.cutoff = static_cast<float>(value);
            } else if (token.text == "res") {
                note.resonance = static_cast<float>(value);
            } else if (token.text == "fine") {
                note.fineTuneCents = static_cast<std::int16_t>(value);
            } else if (token.text == "rel") {
                note.releaseVelocity = static_cast<std::uint16_t>(std::clamp(value, 0.0, 127.0));
            } else {
                m_diag.add(code::kUnknownInlineKey, token.span,
                           "unknown key '" + token.text + "' on a note");
            }
        }
        if (!extra.empty()) {
            extras.push_back(
                project::NoteExtrasAt{.index = notes.size(), .extras = std::move(extra)});
        }
        notes.push_back(note);
        line.claimed = true;
    }

    execute(
        std::make_unique<project::AddNotes>(pattern, channel, std::move(notes), std::move(extras)));
    header.claimed = true;
}

std::vector<project::Breakpoint> V2Parser::parseBreakpoints(const BlockNode& node) {
    std::vector<project::Breakpoint> points;
    core::Ticks previous{std::numeric_limits<std::int64_t>::min()};
    bool unsorted = false;

    for (const BlockNode& child : node.children) {
        Line& line = lines()[child.line];
        const std::vector<Token> fields = tokenize(line, m_diag);
        if (fields.size() < 2 || fields.size() > 3) {
            m_diag.add(code::kWrongFieldCount, line.contentSpan(),
                       "a breakpoint is 'position value [curve]'");
            continue;
        }
        project::Breakpoint point;
        double value = 0.0;
        if (!parsePosition(fields[0].text, m_project.tempo, fields[0].span, m_diag, point.at) ||
            !parseDouble(fields[1].text, fields[1].span, m_diag, value)) {
            continue;
        }
        point.value = static_cast<float>(value);
        if (fields.size() == 3) {
            (void)parseCurve(fields[2].text, fields[2].span, m_diag, point.curve);
        }
        unsorted = unsorted || point.at < previous;
        previous = point.at;
        points.push_back(point);
        line.claimed = true;
    }

    if (unsorted) {
        // v1 assumed the author had sorted them and misbehaved quietly when they had
        // not. Sorting is SetBreakpoints' job; saying so is this one's.
        m_diag.add(code::kBreakpointsUnsorted, lines()[node.line].contentSpan(),
                   "breakpoints were not in ascending order; sorted on load");
    }
    return points;
}

void V2Parser::parseAutomationBlock(core::PatternId pattern, const BlockNode& node) {
    Line& header = lines()[node.line];
    const std::vector<Token> tokens = tokenize(header, m_diag);
    if (tokens.size() != 2) {
        m_diag.add(code::kWrongFieldCount, header.contentSpan(),
                   "AUTOMATION names exactly one parameter path");
        return;
    }
    if (node.children.empty()) {
        m_diag.add(code::kEmptyBlock, header.contentSpan(),
                   "AUTOMATION has no indented body; breakpoints must be indented under it");
    }

    project::AutomationClip clip;
    clip.targetPath = tokens[1].text;
    const project::ParamResolution resolved =
        project::ParamRegistry::resolve(clip.targetPath, m_project);
    if (resolved.ok()) {
        clip.target = resolved.ref;
    } else {
        m_diag.add(code::kUnresolvedParamPath,
                   subSpan(tokens[1].span, resolved.segmentOffset, resolved.segmentLength),
                   "'" + clip.targetPath + "' does not name a parameter that exists");
    }
    clip.points = parseBreakpoints(node);

    execute(std::make_unique<project::AddAutomationClip>(pattern, std::move(clip)));
    header.claimed = true;
}

// --- playlist ----------------------------------------------------------------

void V2Parser::parsePlaylist(Section& section) {
    section.claimed = true;
    const std::vector<BlockNode> nodes = buildBlocks(lines(), section);

    for (const BlockNode& node : nodes) {
        Line& line = lines()[node.line];
        if (line.kind != Line::Kind::Positional) {
            continue;
        }
        const std::vector<Token> tokens = tokenize(line, m_diag);
        if (tokens.empty() || tokens.front().text != "TRACK") {
            continue;
        }
        if (tokens.size() < 2) {
            m_diag.add(code::kWrongFieldCount, line.contentSpan(), "TRACK needs an id");
            continue;
        }
        std::int64_t id = 0;
        if (!parseInt64(tokens[1].text, tokens[1].span, m_diag, id) || id <= 0) {
            m_diag.add(code::kZeroId, tokens[1].span, "a track id must be a positive integer");
            continue;
        }
        project::PlaylistTrack track;
        track.id = core::PlaylistTrackId{static_cast<std::uint32_t>(id)};
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            const Token& token = tokens[i];
            if (!token.isKeyValue()) {
                m_diag.add(code::kWrongFieldCount, token.span, "expected key=value");
                continue;
            }
            if (token.text == "name") {
                track.name = token.value;
            } else if (token.text == "height") {
                std::int64_t height = 1;
                if (parseInt64(token.value, token.valueSpan, m_diag, height)) {
                    track.height =
                        static_cast<std::uint16_t>(std::clamp<std::int64_t>(height, 1, 64));
                }
            } else if (token.text == "mute") {
                bool flag = false;
                if (parseBool(token.value, token.valueSpan, m_diag, flag)) {
                    track.muted = flag;
                }
            } else if (token.text == "color") {
                project::Color color;
                if (parseColor(token.value, token.valueSpan, m_diag, color)) {
                    track.color = color;
                }
            } else {
                m_diag.add(code::kUnknownInlineKey, token.span,
                           "unknown key '" + token.text + "' on a TRACK line");
            }
        }

        auto command = std::make_unique<project::AddPlaylistTrack>(std::move(track));
        const project::AddPlaylistTrack* raw = command.get();
        execute(std::move(command));
        const core::PlaylistTrackId trackId = raw->created();
        line.claimed = true;

        for (const BlockNode& child : node.children) {
            parsePlaylistItem(trackId, child);
        }
    }

    for (const BlockNode& node : nodes) {
        const Line& line = lines()[node.line];
        if (!line.claimed) {
            m_diag.add(code::kUnknownKey, line.contentSpan(),
                       "unrecognised line in [PLAYLIST]; preserved on save");
        }
    }
}

void V2Parser::parsePlaylistItem(core::PlaylistTrackId track, const BlockNode& node) {
    Line& line = lines()[node.line];
    const std::vector<Token> tokens = tokenize(line, m_diag);
    if (tokens.size() < 3) {
        m_diag.add(
            code::kWrongFieldCount, line.contentSpan(),
            "a playlist item is 'PATTERN|AUDIO|AUTOMATION <what> <position> [key=value...]'");
        return;
    }

    project::PlaylistItem item;
    const std::string& verb = tokens.front().text;

    if (verb == "PATTERN") {
        const project::Pattern* pattern = m_project.findPatternByName(tokens[1].text);
        if (pattern == nullptr) {
            m_diag.add(code::kUnknownPatternRef, tokens[1].span,
                       "no pattern named '" + tokens[1].text + "'");
            return;
        }
        item.content = project::PatternRef{.pattern = pattern->id};
    } else if (verb == "AUDIO") {
        auto sample = std::make_unique<project::AddSample>(tokens[1].text);
        const project::AddSample* raw = sample.get();
        execute(std::move(sample));
        item.content = project::AudioClipRef{.sample = raw->created()};
    } else if (verb == "AUTOMATION") {
        project::AutomationClip clip;
        clip.targetPath = tokens[1].text;
        const project::ParamResolution resolved =
            project::ParamRegistry::resolve(clip.targetPath, m_project);
        if (resolved.ok()) {
            clip.target = resolved.ref;
        } else {
            m_diag.add(code::kUnresolvedParamPath,
                       subSpan(tokens[1].span, resolved.segmentOffset, resolved.segmentLength),
                       "'" + clip.targetPath + "' does not name a parameter that exists");
        }
        clip.points = parseBreakpoints(node);
        auto command =
            std::make_unique<project::AddAutomationClip>(core::PatternId{}, std::move(clip));
        const project::AddAutomationClip* raw = command.get();
        execute(std::move(command));
        item.content = project::AutomationRef{.clip = raw->created()};
    } else {
        m_diag.add(code::kUnknownKey, line.contentSpan(),
                   "unrecognised playlist item; preserved on save");
        return;
    }

    if (!parsePosition(tokens[2].text, m_project.tempo, tokens[2].span, m_diag, item.start)) {
        return;
    }

    auto* audio = std::get_if<project::AudioClipRef>(&item.content);
    for (std::size_t i = 3; i < tokens.size(); ++i) {
        const Token& token = tokens[i];
        if (!token.isKeyValue()) {
            m_diag.add(code::kWrongFieldCount, token.span, "expected key=value");
            continue;
        }
        if (token.text == "length") {
            (void)parseDuration(token.value, m_project.tempo, token.valueSpan, m_diag, item.length);
        } else if (token.text == "offset") {
            (void)parseDuration(token.value, m_project.tempo, token.valueSpan, m_diag,
                                item.sourceOffset);
        } else if (token.text == "mute") {
            bool flag = false;
            if (parseBool(token.value, token.valueSpan, m_diag, flag)) {
                item.muted = flag;
            }
        } else if (audio != nullptr && token.text == "stretch") {
            double value = 1.0;
            if (parseDouble(token.value, token.valueSpan, m_diag, value)) {
                audio->stretch = static_cast<float>(value);
            }
        } else if (audio != nullptr && token.text == "pitch") {
            double value = 0.0;
            if (parseDouble(token.value, token.valueSpan, m_diag, value)) {
                audio->pitchSemitones = static_cast<float>(value);
            }
        } else if (audio != nullptr && token.text == "reverse") {
            bool flag = false;
            if (parseBool(token.value, token.valueSpan, m_diag, flag)) {
                audio->reverse = flag;
            }
        } else {
            m_diag.add(code::kUnknownInlineKey, token.span,
                       "unknown key '" + token.text + "' on a playlist item");
        }
    }

    if (!std::holds_alternative<project::AutomationRef>(item.content)) {
        // An automation item's children are its breakpoints; anything else's are its
        // clip envelopes (phase_4.md §4.0).
        for (const BlockNode& child : node.children) {
            parseClipEnvelope(child, item);
        }
    }

    execute(std::make_unique<project::AddPlaylistItem>(track, item));
    line.claimed = true;
}

void V2Parser::checkParam(const project::TypeInfo* type, const Token& token, double value) {
    const project::ParamDescriptor* descriptor =
        type != nullptr ? project::findParam(*type, token.text)
                        : project::ParamRegistry::describeNamed(token.text);
    if (descriptor == nullptr) {
        m_diag.add(code::kUnknownParameter, token.span,
                   type != nullptr ? "'" + std::string(type->name) + "' has no parameter '" +
                                         token.text + "'; kept"
                                   : "unknown parameter '" + token.text + "'; kept");
        return;
    }
    if (value < descriptor->minimum || value > descriptor->maximum) {
        m_diag.add(code::kValueOutOfRange, token.valueSpan,
                   "'" + token.text + "' is " + formatDouble(value) + ", outside " +
                       formatFloat(descriptor->minimum) + ".." + formatFloat(descriptor->maximum) +
                       "; clamped when played");
    }
}

void V2Parser::parseClipEnvelope(const BlockNode& node, project::PlaylistItem& item) {
    Line& header = lines()[node.line];
    const std::vector<Token> tokens = tokenize(header, m_diag);
    if (tokens.empty() || tokens.front().text != "ENVELOPE") {
        m_diag.add(code::kUnknownKey, header.contentSpan(),
                   "unrecognised line under a playlist item; preserved on save");
        return;
    }
    if (tokens.size() != 2) {
        m_diag.add(code::kWrongFieldCount, header.contentSpan(),
                   "ENVELOPE names exactly one target: gain, pan, pitch or a parameter path");
        return;
    }
    project::ClipEnvelope envelope;
    if (!project::clipTargetFromString(tokens[1].text, envelope.local)) {
        envelope.local = project::ClipTarget::Param;
        envelope.targetPath = tokens[1].text;
        const project::ParamResolution resolved =
            project::ParamRegistry::resolve(envelope.targetPath, m_project);
        if (resolved.ok()) {
            envelope.target = resolved.ref;
        } else {
            m_diag.add(code::kUnresolvedParamPath,
                       subSpan(tokens[1].span, resolved.segmentOffset, resolved.segmentLength),
                       "'" + envelope.targetPath + "' does not name a parameter that exists");
        }
    }
    if (node.children.empty()) {
        m_diag.add(code::kEmptyBlock, header.contentSpan(),
                   "ENVELOPE has no indented body; breakpoints must be indented under it");
    }
    envelope.points = parseBreakpoints(node);
    std::ranges::stable_sort(envelope.points, {}, &project::Breakpoint::at);
    item.envelopes.push_back(std::move(envelope));
    header.claimed = true;
}

void V2Parser::run() {
    if (m_options.groupAsOneStep) {
        m_stack.beginGroup("Load project");
    }

    // Sections are handled in dependency order, not file order, so a forward
    // reference never needs a second pass and a file may list its sections however
    // its author likes.
    const auto forEach = [this](std::string_view type, auto&& handler) {
        for (Section& section : m_doc.sections()) {
            if (section.type == type) {
                handler(section);
            }
        }
    };

    forEach("METER", [this](Section& s) { parseMeter(s); });
    forEach("TEMPO", [this](Section& s) { parseTempo(s); });
    forEach("PROJECT", [this](Section& s) { parseProject(s); });
    forEach("MIXER", [this](Section& s) { parseMixer(s); });

    // A file that declares no inserts still needs somewhere for its channels to go.
    // Creating the master here rather than in Project's constructor keeps "a new
    // project is empty" true, and keeps every entity's existence traceable to a
    // command.
    if (!m_project.mixer.master.valid()) {
        project::Insert master;
        master.name = "Master";
        execute(std::make_unique<project::AddInsert>(std::move(master)));
    }

    for (std::size_t i = 0; i < m_doc.sections().size(); ++i) {
        Section& section = m_doc.sections()[i];
        if (section.type != "CHANNEL") {
            continue;
        }
        ChannelBinding binding = parseChannelHeader(section, i);
        if (binding.id.valid()) {
            m_channels.push_back(std::move(binding));
        }
    }
    for (const ChannelBinding& binding : m_channels) {
        parseChannelOutput(binding);
    }

    for (std::size_t i = 0; i < m_doc.sections().size(); ++i) {
        Section& section = m_doc.sections()[i];
        if (section.type != "PATTERN") {
            continue;
        }
        const core::PatternId id = parsePatternHeader(section);
        if (id.valid()) {
            m_patterns.push_back(PatternBinding{.section = i, .id = id});
        }
    }
    for (const PatternBinding& binding : m_patterns) {
        parsePatternBody(binding);
    }

    forEach("PLAYLIST", [this](Section& s) { parsePlaylist(s); });
    forEach("MARKERS", [this](Section& s) { parseMarkers(s); });

    for (const Section& section : m_doc.sections()) {
        if (!section.isPreamble() && !section.claimed) {
            reportUnknownSection(section);
        }
    }

    m_project.resources.baseDirectory = m_options.baseDirectory;
    m_project.residue = m_doc.residue();

    if (m_options.groupAsOneStep) {
        m_stack.endGroup();
    }
}

} // namespace

bool isVersion2(const Document& document) noexcept {
    for (const Section& section : document.sections()) {
        if (section.type != "PROJECT") {
            continue;
        }
        for (const std::size_t index : section.lineIndices) {
            const Line& line = document.lines()[index];
            if (line.kind != Line::Kind::KeyValue || upper(line.key) != "ADX_VERSION") {
                continue;
            }
            std::int64_t version = 0;
            const char* begin = line.value.data();
            const char* end = begin + line.value.size();
            const auto result = std::from_chars(begin, end, version);
            return result.ec == std::errc{} && version >= 2;
        }
    }
    return false;
}

void parseV2(Document& document, Project& project, project::CommandStack& stack,
             DiagnosticList& diagnostics, const ParseOptions& options) {
    V2Parser parser(document, project, stack, diagnostics, options);
    parser.run();
}

void load(std::string_view text, Project& project, project::CommandStack& stack,
          DiagnosticList& diagnostics, const ParseOptions& options) {
    Document document = Document::parse(text, diagnostics);
    if (isVersion2(document)) {
        parseV2(document, project, stack, diagnostics, options);
    } else {
        migrateV1(document, project, stack, diagnostics, options);
    }
    diagnostics.sortByPosition();
}

} // namespace adx::format
