// The lexing discipline, and the one rule in the whole format that silently
// destroys user data when it is wrong.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "engine/format/adx/Lexer.h"

using adx::format::DiagnosticList;
using adx::format::Line;
using adx::format::Token;

namespace {

[[nodiscard]] std::vector<Line> lex(std::string_view text, DiagnosticList& diagnostics) {
    std::string bom;
    return adx::format::lexLines(text, bom, diagnostics);
}

[[nodiscard]] Line lexOne(std::string_view text, DiagnosticList& diagnostics) {
    std::vector<Line> lines = lex(text, diagnostics);
    REQUIRE(lines.size() == 1);
    return lines.front();
}

} // namespace

TEST_CASE("lexer_hash_comment_vs_sharp", "[lexer]") {
    // Ported verbatim from iteration one, which got this exactly right: `#` starts a
    // comment only where a token could start. Getting it wrong turns `F#5` into `F`.
    DiagnosticList diagnostics;
    const Line line = lexOne("F#5 0:0:0 0:1:0 96  # the comment", diagnostics);

    CHECK(line.kind == Line::Kind::Positional);
    CHECK(line.content == "F#5 0:0:0 0:1:0 96");

    const std::vector<Token> tokens = adx::format::tokenize(line, diagnostics);
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[0].text == "F#5");
    CHECK(tokens[3].text == "96");

    // And a line that is nothing but a comment is a comment.
    CHECK(lexOne("  # just a note to self", diagnostics).kind == Line::Kind::Comment);
    CHECK(lexOne("#no space needed", diagnostics).kind == Line::Kind::Comment);
}

TEST_CASE("lexer_hash_in_quotes", "[lexer]") {
    DiagnosticList diagnostics;
    const Line line = lexOne("AUDIO \"track #3.wav\" 16:0:0", diagnostics);
    CHECK(line.content == "AUDIO \"track #3.wav\" 16:0:0");

    const std::vector<Token> tokens = adx::format::tokenize(line, diagnostics);
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[1].text == "track #3.wav");
    CHECK(tokens[1].quoted);
    CHECK(diagnostics.empty());
}

TEST_CASE("lexer_escaped_hash", "[lexer]") {
    DiagnosticList diagnostics;
    const Line line = lexOne("AUDIO take\\#3.wav 0:0:0", diagnostics);
    CHECK(line.content == "AUDIO take\\#3.wav 0:0:0");

    const std::vector<Token> tokens = adx::format::tokenize(line, diagnostics);
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[1].text == "take#3.wav");
    CHECK(diagnostics.empty());
}

TEST_CASE("an unrecognised escape keeps both characters", "[lexer]") {
    // A Windows path that slipped through unquoted is the likely cause, and eating
    // its separators would be the worst possible response.
    DiagnosticList diagnostics;
    const Line line = lexOne("AUDIO C:\\samples\\kick.wav 0:0:0", diagnostics);
    const std::vector<Token> tokens = adx::format::tokenize(line, diagnostics);
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[1].text == "C:\\samples\\kick.wav");
    CHECK(diagnostics.count(adx::format::Severity::Warning) > 0);
}

TEST_CASE("a quoted run inside a bare word keeps its quotes", "[lexer]") {
    // This is what makes `channel."Hardstyle Kick".cutoff` one token: the path
    // splitter still needs to see where the quoted segment began.
    DiagnosticList diagnostics;
    const Line line = lexOne("AUTOMATION channel.\"Hardstyle Kick\".drive", diagnostics);
    const std::vector<Token> tokens = adx::format::tokenize(line, diagnostics);
    REQUIRE(tokens.size() == 2);
    CHECK(tokens[1].text == "channel.\"Hardstyle Kick\".drive");
}

TEST_CASE("key=value is only a key=value when the key is an identifier", "[lexer]") {
    DiagnosticList diagnostics;
    CHECK(lexOne("TITLE=\"Suffocation\"", diagnostics).kind == Line::Kind::KeyValue);
    CHECK(lexOne("TITLE = \"Suffocation\"", diagnostics).kind == Line::Kind::KeyValue);

    // `PARAM env.attack=0.01` stays positional, which is what puts its inline pairs
    // through the token path rather than making `PARAM env` a key.
    const Line param = lexOne("PARAM env.attack=0.01 curve=smooth", diagnostics);
    CHECK(param.kind == Line::Kind::Positional);

    const std::vector<Token> tokens = adx::format::tokenize(param, diagnostics);
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0].text == "PARAM");
    CHECK(tokens[1].isKeyValue());
    CHECK(tokens[1].text == "env.attack");
    CHECK(tokens[1].value == "0.01");
    CHECK(tokens[2].text == "curve");
    CHECK(tokens[2].value == "smooth");
}

TEST_CASE("an inline value may be quoted and contain spaces", "[lexer]") {
    DiagnosticList diagnostics;
    const Line line = lexOne("TRACK 1 name=\"Drums and things\" height=2", diagnostics);
    const std::vector<Token> tokens = adx::format::tokenize(line, diagnostics);
    REQUIRE(tokens.size() == 4);
    CHECK(tokens[2].text == "name");
    CHECK(tokens[2].value == "Drums and things");
    CHECK(tokens[3].value == "2");
}

TEST_CASE("section headers keep their argument verbatim", "[lexer]") {
    DiagnosticList diagnostics;
    const Line line = lexOne("[CHANNEL Hardstyle Kick]", diagnostics);
    CHECK(line.kind == Line::Kind::SectionHeader);
    CHECK(line.sectionType == "CHANNEL");
    CHECK(line.sectionName == "Hardstyle Kick");

    const Line bare = lexOne("[PLAYLIST]", diagnostics);
    CHECK(bare.sectionType == "PLAYLIST");
    CHECK(bare.sectionName.empty());
}

TEST_CASE("indentation is measured, and is the only thing that nests", "[lexer]") {
    DiagnosticList diagnostics;
    const std::vector<Line> lines = lex("NOTES Lead\n  C4 0:0:0 0:1:0 96\n", diagnostics);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].indent == 0);
    CHECK(lines[1].indent == 2);
    CHECK(lines[1].contentColumn == 3);
}

TEST_CASE("an unterminated string is an error, not a truncation", "[lexer]") {
    DiagnosticList diagnostics;
    const Line line = lexOne("AUDIO \"unfinished 0:0:0", diagnostics);
    const std::vector<Token> tokens = adx::format::tokenize(line, diagnostics);
    REQUIRE(tokens.size() == 2);
    CHECK(tokens[1].text == "unfinished 0:0:0");
    CHECK(diagnostics.hasErrors());
}
