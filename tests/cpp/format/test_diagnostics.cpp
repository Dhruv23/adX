// Diagnostics: every code documented, and every one of them addressable.
//
// Iteration one's diagnostics all degraded to "Malformed X on line N" because
// std::exception::what() carries no position. The column test below is what keeps
// that from happening again - a diagnostic an editor cannot underline is barely
// better than no diagnostic.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Parser.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "tests/cpp/Corpus.h"

using adx::format::DiagnosticList;
using adx::format::Severity;

namespace {

struct Expectation {
    std::string text;
    std::uint16_t code{0};
    std::uint32_t line{0};
    std::uint32_t column{0};
    std::uint32_t length{0};
};

/// Parses `text` and returns the first diagnostic carrying `code`, or nullptr.
[[nodiscard]] bool findAt(const std::string& text, const Expectation& expected,
                          adx::format::Diagnostic& out) {
    adx::project::Project project;
    adx::project::CommandStack stack;
    DiagnosticList diagnostics;
    adx::format::load(text, project, stack, diagnostics);
    for (const auto& item : diagnostics.all()) {
        if (item.code == expected.code) {
            out = item;
            return true;
        }
    }
    return false;
}

/// A v2 preamble, so the file goes through the v2 parser rather than the v1 shim.
const std::string kHeader = "[PROJECT]\nADX_VERSION=2\n";

} // namespace

TEST_CASE("diagnostics_all_codes_documented", "[format][diagnostics]") {
    // Every code this build can emit appears in the spec. The table in
    // Diagnostics.cpp is the only way to emit one, so this covers all of them.
    const std::string spec =
        adx::tests::readFile(adx::tests::repoRoot() / "docs" / "adx-format-v2.md");
    REQUIRE_FALSE(spec.empty());

    for (const auto& row : adx::format::allDiagnosticCodes()) {
        adx::format::Diagnostic probe;
        probe.code = row.code;
        const std::string spelled = probe.codeString();
        INFO(spelled << " - " << row.summary);
        CHECK(spec.find(spelled) != std::string::npos);
    }
}

TEST_CASE("every documented code exists in the table", "[format][diagnostics]") {
    // The other direction: the spec must not document a code nothing can emit, or
    // `adx validate --explain ADX2099` would have nothing to say.
    const std::string spec =
        adx::tests::readFile(adx::tests::repoRoot() / "docs" / "adx-format-v2.md");
    REQUIRE_FALSE(spec.empty());

    std::size_t at = 0;
    std::size_t checked = 0;
    while ((at = spec.find("ADX", at)) != std::string::npos) {
        const std::string token = spec.substr(at, 7);
        at += 3;
        if (token.size() != 7) {
            continue;
        }
        bool digits = true;
        for (std::size_t i = 3; i < 7; ++i) {
            digits = digits && token[i] >= '0' && token[i] <= '9';
        }
        if (!digits) {
            continue;
        }
        const auto code = static_cast<std::uint16_t>(std::stoi(token.substr(3)));
        INFO(token);
        CHECK(adx::format::findDiagnosticCode(code) != nullptr);
        ++checked;
    }
    CHECK(checked > 40);
}

TEST_CASE("diagnostics_column_accuracy", "[format][diagnostics]") {
    using adx::format::code::kBadNoteName;
    using adx::format::code::kMalformedNumber;
    using adx::format::code::kMalformedPosition;
    using adx::format::code::kMalformedReference;
    using adx::format::code::kUnclosedSectionHeader;
    using adx::format::code::kUnknownChannelRef;
    using adx::format::code::kUnknownCurve;
    using adx::format::code::kUnknownInlineKey;
    using adx::format::code::kUnknownInsertRef;
    using adx::format::code::kUnknownKey;
    using adx::format::code::kUnknownSection;
    using adx::format::code::kUnterminatedString;
    using adx::format::code::kWrongFieldCount;

    // Every column below was worked out from the text by hand. A `length` of zero
    // means the case does not pin one down.
    // NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table:
    // one row per line, columns aligned by position. Designating every field would
    // triple its width and bury the values the table exists to show.
    const std::vector<Expectation> cases{
        // `TUNING=abc` - the value starts at column 8 and all three characters are bad.
        {kHeader + "TUNING=abc\n", kMalformedNumber, 3, 8, 3},
        // `TUNING=44a` - from_chars consumes `44`, so the underline is the `a` alone.
        {kHeader + "TUNING=44a\n", kMalformedNumber, 3, 10, 1},
        // A header with no closing bracket.
        {"[PROJECT\n", kUnclosedSectionHeader, 1, 1, 8},
        // An unterminated string: the token starts at column 7 and runs to the end.
        {kHeader + "TITLE=\"abc\n", kUnterminatedString, 3, 7, 4},
        // An unknown key is reported on the key, not the value.
        {kHeader + "NOT_A_KEY=1\n", kUnknownKey, 3, 1, 9},
        // An unknown section is reported on the header.
        {kHeader + "\n[NONSENSE]\nx=1\n", kUnknownSection, 4, 1, 10},
        // A tempo line with a malformed position.
        {kHeader + "\n[TEMPO]\n0:x:0 120\n", kMalformedPosition, 5, 1, 5},
        // A tempo line with the wrong number of fields.
        {kHeader + "\n[TEMPO]\n0:0:0\n", kWrongFieldCount, 5, 1, 5},
        // A bad curve name on a breakpoint, in the third field.
        {kHeader + "\n[CHANNEL Lead]\nPARAM drive=1 curve=wobble\n", kUnknownCurve, 5, 21, 6},
        // An unknown inline key on a PARAM line.
        {kHeader + "\n[CHANNEL Lead]\nPARAM drive=1 nonsense=2\n", kUnknownInlineKey, 5, 15, 10},
        // An insert reference to something that does not exist.
        {kHeader + "\n[CHANNEL Lead]\nOUTPUT=insert.9\n", kUnknownInsertRef, 5, 8, 8},
        // A malformed reference.
        {kHeader + "\n[CHANNEL Lead]\nOUTPUT=banana\n", kMalformedReference, 5, 8, 6},
        // A pattern naming a channel that is not there.
        {kHeader + "\n[PATTERN P]\nNOTES Ghost\n  C4 0:0:0 0:1:0 96\n", kUnknownChannelRef, 5, 7,
         5},
        // A note name that is not one.
        {kHeader + "\n[CHANNEL Lead]\n\n[PATTERN P]\nNOTES Lead\n  H4 0:0:0 0:1:0 96\n",
         kBadNoteName, 8, 3, 1},
        // A route with the wrong shape.
        {kHeader + "\n[MIXER]\nINSERT 1 name=\"M\"\nROUTE insert.1 insert.1\n", kWrongFieldCount, 6,
         1, 23},
        // A velocity that is not a number.
        {kHeader + "\n[CHANNEL Lead]\n\n[PATTERN P]\nNOTES Lead\n  C4 0:0:0 0:1:0 loud\n",
         kMalformedNumber, 8, 18, 4},
        // A meter denominator that does not divide a whole note.
        {kHeader + "\n[METER]\n0:0:0 4/5\n", adx::format::code::kBadMeterDenominator, 5, 7, 3},
        // A duplicate key.
        {kHeader + "TUNING=440\nTUNING=441\n", adx::format::code::kDuplicateKey, 3, 1, 6},
        // An out-of-range polyphony.
        {kHeader + "\n[CHANNEL Lead]\nPOLYPHONY=9999\n", adx::format::code::kValueOutOfRange, 5, 11,
         4},
        // A block opener with nothing indented under it.
        {kHeader + "\n[CHANNEL Lead]\n\n[PATTERN P]\nNOTES Lead\n", adx::format::code::kEmptyBlock,
         7, 1, 10},
    };
    // NOLINTEND(modernize-use-designated-initializers)

    for (const Expectation& expected : cases) {
        adx::format::Diagnostic actual;
        INFO("input:\n" << expected.text);
        REQUIRE(findAt(expected.text, expected, actual));
        CHECK(actual.span.line == expected.line);
        CHECK(actual.span.column == expected.column);
        if (expected.length != 0) {
            CHECK(actual.span.length == expected.length);
        }
    }
}

TEST_CASE("a code has exactly one severity", "[format][diagnostics]") {
    // The severity comes from the table rather than from the call site, so one code
    // cannot be an error in one place and a warning in another.
    DiagnosticList diagnostics;
    diagnostics.add(adx::format::code::kUnknownKey, {}, "first");
    diagnostics.add(adx::format::code::kUnknownKey, {}, "second");
    CHECK(diagnostics.count(Severity::Warning) == 2);
    CHECK_FALSE(diagnostics.hasErrors());
}

TEST_CASE("codes are spelled with four digits", "[format][diagnostics]") {
    adx::format::Diagnostic item;
    item.code = 1;
    CHECK(item.codeString() == "ADX0001");
    item.code = 4012;
    CHECK(item.codeString() == "ADX4012");
}
