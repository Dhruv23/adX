// Incremental reparse and the editor's token stream (phase_7.md §4.3, §4.6, §6).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "engine/format/adx/Incremental.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Tokenize.h"
#include "engine/project/Project.h"
#include "engine/project/SourceMap.h"
#include "engine/project/Validate.h"
#include "engine/project/commands/CommandStack.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::format::IncrementalParse;

namespace {

using Key = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint16_t, std::string>;

std::vector<Key> keysOf(const adx::format::DiagnosticList& diagnostics) {
    std::vector<Key> keys;
    for (const auto& d : diagnostics.all()) {
        keys.emplace_back(d.span.line, d.span.column, d.span.length, d.code, d.message);
    }
    std::ranges::sort(keys);
    return keys;
}

/// A full parse and validation - what the editor shows (Incremental.h).
std::vector<Key> fullKeys(const std::string& text) {
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::project::SourceMap sources;
    adx::format::DiagnosticList diagnostics;
    adx::format::ParseOptions options;
    options.sources = &sources;
    adx::format::load(text, project, stack, diagnostics, options);
    adx::project::ValidationOptions validation;
    validation.sources = &sources;
    static_cast<void>(adx::project::validate(project, diagnostics, validation));
    return keysOf(diagnostics);
}

/// A file of about `lines` lines: a few channels and a mixer, then patterns of notes -
/// the shape of a real file big enough to need incremental parsing.
std::string bigFile(int lines) {
    std::string text = "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 128\n\n"
                       "[CHANNEL Lead]\nOUTPUT=insert.1\n\n[CHANNEL Bass]\nOUTPUT=insert.1\n\n"
                       "[CHANNEL Drums]\nOUTPUT=insert.1\n\n";
    const std::array<const char*, 6> names{"C4", "D4", "E4", "G4", "A4", "C5"};
    int pattern = 0;
    int written = 12;
    std::string playlist = "[PLAYLIST]\nTRACK 1\n";
    while (written < lines - 20) {
        const std::string name = "P" + std::to_string(pattern);
        text += "[PATTERN " + name + "]\nLENGTH=16:0:0\nNOTES Lead\n";
        written += 3;
        for (int n = 0; n < 240 && written < lines - 20; ++n, ++written) {
            text += "  " + std::string(names.at(static_cast<std::size_t>(n % 6))) + " " +
                    std::to_string(n / 16) + ":" + std::to_string((n / 4) % 4) + ":" +
                    std::to_string((n % 4) * 960) + " 0:0:960 " + std::to_string(60 + (n % 60)) +
                    "\n";
        }
        text += "MINI Drums\n  [bd sn]*2 hh(3,8)\n\n";
        written += 3;
        playlist += "  PATTERN " + name + " " + std::to_string(pattern * 16) + ":0:0\n";
        ++pattern;
    }
    return text + playlist + "\n[MIXER]\nINSERT 1 name=\"Master\"\n";
}

std::vector<std::string> linesOf(const std::string& text) {
    return adx::format::splitLines(text);
}

std::string join(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& line : lines) {
        out += line;
    }
    return out;
}

/// One random edit of the kinds a person makes while typing.
void randomEdit(std::vector<std::string>& lines, std::mt19937& rng) {
    if (lines.empty()) {
        lines.emplace_back("\n");
        return;
    }
    std::uniform_int_distribution<std::size_t> pick(0, lines.size() - 1);
    const std::size_t at = pick(rng);
    std::string& line = lines[at];
    const auto chars = std::uniform_int_distribution<int>(0, 9)(rng);
    const std::string garbage = "x9:@[]= #\"";
    switch (std::uniform_int_distribution<int>(0, 7)(rng)) {
    case 0: // type a character
    case 1: {
        const std::size_t pos =
            std::uniform_int_distribution<std::size_t>(0, line.empty() ? 0 : line.size() - 1)(rng);
        line.insert(pos, 1, garbage[static_cast<std::size_t>(chars)]);
        break;
    }
    case 2: // delete a character
        if (line.size() > 1) {
            line.erase(std::uniform_int_distribution<std::size_t>(0, line.size() - 2)(rng), 1);
        }
        break;
    case 3: // a new note line
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), "  B4 0:3:0 0:0:960 77\n");
        break;
    case 4: // delete a line
        lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(at));
        break;
    case 5: // duplicate a line
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
        break;
    case 6: // change a digit
        for (char& c : line) {
            if (c >= '0' && c <= '8') {
                ++c;
                break;
            }
        }
        break;
    default: // retype a line as something else entirely
        line =
            std::uniform_int_distribution<int>(0, 1)(rng) == 0 ? "[PATTERN New]\n" : "  C4 0:0:0\n";
        break;
    }
}

} // namespace

TEST_CASE("incremental_matches_full", "[sync][incremental]") {
    // After every one of thousands of random edits, the incremental diagnostics are
    // exactly a full parse's.
    const int edits = adx::tests::optimisedBuild() ? 10000 : 1500;
    std::string const base = bigFile(400);
    std::vector<std::string> lines = linesOf(base);
    IncrementalParse incremental;
    incremental.reset(base);
    REQUIRE(keysOf(incremental.diagnostics()) == fullKeys(base));
    std::mt19937 rng(43U);
    int cheap = 0;
    for (int i = 0; i < edits; ++i) {
        randomEdit(lines, rng);
        const std::string text = join(lines);
        incremental.update(text);
        cheap += incremental.lastWasIncremental() ? 1 : 0;
        const auto expected = fullKeys(text);
        const auto actual = keysOf(incremental.diagnostics());
        if (actual != expected) {
            INFO("edit " << i << ": " << actual.size() << " diagnostics, expected "
                         << expected.size());
            REQUIRE(actual == expected);
        }
        if (i % 500 == 499) {
            // Keep the file from drifting into nothing but errors.
            lines = linesOf(base);
            incremental.update(join(lines));
        }
    }
    INFO(cheap << " of " << edits << " edits took the incremental path");
    CHECK(cheap > edits / 3);
}

TEST_CASE("incremental_budget", "[sync][incremental]") {
    // < 5 ms from keystroke to updated diagnostics on a 10,000-line file, for the
    // keystroke that matters: typing inside a pattern.
    const std::string base = bigFile(10000);
    std::vector<std::string> lines = linesOf(base);
    REQUIRE(lines.size() >= 9900);
    IncrementalParse incremental;
    incremental.reset(base);
    const double fullMs = incremental.lastMilliseconds();
    double worst = 0.0;
    for (int key = 0; key < 20; ++key) {
        // Somewhere in the middle of the file: a velocity being retyped.
        std::string& line = lines[(lines.size() / 2) + static_cast<std::size_t>(key * 7)];
        REQUIRE(line.starts_with("  "));
        line.insert(line.size() - 1, 1, static_cast<char>('0' + (key % 10)));
        incremental.update(join(lines));
        CHECK(incremental.lastWasIncremental());
        worst = std::max(worst, incremental.lastMilliseconds());
    }
    INFO("10,000 lines: full parse " << fullMs << " ms, worst keystroke " << worst << " ms");
    CHECK(keysOf(incremental.diagnostics()) == fullKeys(join(lines)));
    if (adx::tests::optimisedBuild()) {
        CHECK(worst < 5.0);
    }
}

TEST_CASE("semantic_tokens_follow_the_grammar", "[sync][tokens]") {
    const std::string text = "[PATTERN Verse]\nLENGTH=1:0:0  # a bar\nNOTES Lead\n"
                             "  F#4 0:0:0 0:1:0 100 lyric=\"la\"\nMINI Drums\n"
                             "  [bd sn]*2 ~ hh(3,8) | rev\n\n[MIXER]\nROUTE insert.2 -> insert.1\n";
    const auto tokens = adx::format::semanticTokens(text);
    const auto kindAt = [&](std::uint32_t line, std::uint32_t column) {
        for (const auto& t : tokens) {
            if (t.line == line && column >= t.column && column < t.column + t.length) {
                return std::string(adx::format::toString(t.kind));
            }
        }
        return std::string("none");
    };
    CHECK(kindAt(1, 0) == "section-bracket");
    CHECK(kindAt(1, 1) == "section-type");
    CHECK(kindAt(1, 9) == "section-name");
    CHECK(kindAt(2, 0) == "key");
    CHECK(kindAt(2, 6) == "operator");
    CHECK(kindAt(2, 7) == "position");
    CHECK(kindAt(2, 14) == "comment");
    CHECK(kindAt(3, 0) == "keyword");
    CHECK(kindAt(3, 6) == "identifier");
    CHECK(kindAt(4, 2) == "note");
    CHECK(kindAt(4, 6) == "position");
    CHECK(kindAt(4, 18) == "number");
    CHECK(kindAt(4, 22) == "key");
    CHECK(kindAt(4, 28) == "string");
    CHECK(kindAt(5, 0) == "keyword");
    // The MINI body is mini-notation - the line starting with '[' included.
    CHECK(kindAt(6, 2) == "mini-operator");
    CHECK(kindAt(6, 3) == "mini-step");
    CHECK(kindAt(6, 10) == "mini-number");
    CHECK(kindAt(6, 12) == "mini-rest");
    CHECK(kindAt(6, 14) == "mini-step");
    CHECK(kindAt(6, 17) == "mini-number");
    CHECK(kindAt(6, 24) == "mini-transform");
    CHECK(kindAt(9, 0) == "keyword");
    CHECK(kindAt(9, 6) == "reference");
    CHECK(kindAt(9, 15) == "operator");
    // Tokens never overlap and are in order.
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const auto& a = tokens[i - 1];
        const auto& b = tokens[i];
        CHECK((a.line < b.line || (a.line == b.line && a.column + a.length <= b.column)));
    }
}

TEST_CASE("mini_line_starting_with_a_group_is_not_a_section", "[sync][tokens][format]") {
    // `  [bd sn] hh` under MINI is mini-notation, not a section header.
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(
        "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL Drums]\nOUTPUT=insert.1\n\n"
        "[PATTERN Beat]\nMINI Drums\n  [bd sn] hh\n\n[MIXER]\nINSERT 1 name=\"Master\"\n",
        project, stack, diagnostics);
    CHECK_FALSE(diagnostics.hasErrors());
    REQUIRE(project.patterns.size() == 1);
    REQUIRE(project.patterns[0].mini.size() == 1);
    CHECK(project.patterns[0].mini[0].lines == std::vector<std::string>{"[bd sn] hh"});
    CHECK(project.residue.empty());
}
