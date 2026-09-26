// The foundation everything in engine/format stands on:
//
//     Document::parse(text).write() == text
//
// byte for byte, for **any** input including malformed ones. Checked against the
// corpus and against ten thousand fuzzer-generated inputs.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "engine/format/adx/Document.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "tests/cpp/Corpus.h"

using adx::format::DiagnosticList;
using adx::format::Document;

namespace {

/// Fragments the generator splices together.
///
/// Chosen to hit the cases §9 of phase_2.md names as the real hazards - CRLF, BOM,
/// trailing whitespace - plus the ones that have broken line-oriented parsers
/// before: a lone CR, a NUL byte, a header with no closing bracket, an unterminated
/// quote, and a file that does not end in a newline.
const std::vector<std::string>& fragments() {
    static const std::vector<std::string> kFragments{
        "[PROJECT]",
        "[MIXER]",
        "[CHANNEL Lead]",
        "[UNKNOWN thing]",
        "[",
        "[]",
        "TITLE=\"x\"",
        "TITLE=",
        "=value",
        "PARAM a.b=1",
        "  indented",
        "\tTabbed",
        "# comment",
        "",
        "F#5 0:0:0 0:1:0 96  # tail",
        "AUDIO \"a b.wav\" 0:0:0",
        "AUDIO \"unterminated",
        "back\\slash",
        "trailing   ",
        "\xEF\xBB\xBF",
        "\r",
        // A NUL byte. Built rather than written as a literal, because a literal
        // containing one is silently truncated there.
        std::string("nul") + std::string(1, '\0') + std::string("byte"),
        "NOTES Lead",
        "ROUTE insert.1 -> insert.2",
        "0:0:0 120.0 ramp",
    };
    return kFragments;
}

[[nodiscard]] std::string generate(std::mt19937& rng) {
    std::uniform_int_distribution<std::size_t> pick(0, fragments().size() - 1);
    std::uniform_int_distribution<int> lineCount(0, 12);
    std::uniform_int_distribution<int> ending(0, 2);

    std::string out;
    const int count = lineCount(rng);
    for (int i = 0; i < count; ++i) {
        const std::string& fragment = fragments()[pick(rng)];
        out.append(fragment.data(), fragment.size());
        switch (ending(rng)) {
        case 0:
            out += "\n";
            break;
        case 1:
            out += "\r\n";
            break;
        default:
            // No terminator: the next fragment runs straight on, which is how a last
            // line without a newline gets exercised in the middle of a file too.
            break;
        }
    }
    return out;
}

} // namespace

TEST_CASE("document_byte_identical_roundtrip corpus", "[format][roundtrip]") {
    for (const auto& path : adx::tests::corpusPaths()) {
        INFO(path.string());
        const std::string text = adx::tests::readFile(path);
        REQUIRE_FALSE(text.empty());

        DiagnosticList diagnostics;
        const Document document = Document::parse(text, diagnostics);
        CHECK(document.write() == text);
    }
}

TEST_CASE("document_byte_identical_roundtrip fuzz", "[format][roundtrip]") {
    // Fixed seed, fixed case count: fast and deterministic, so this runs on every CI
    // build rather than only in a nightly job. The nightly random run is a separate
    // thing and does not replace this one.
    std::mt19937 rng(20260913U);
    for (int i = 0; i < 10000; ++i) {
        const std::string text = generate(rng);
        DiagnosticList diagnostics;
        const Document document = Document::parse(text, diagnostics);
        const std::string written = document.write();
        if (written != text) {
            INFO("case " << i << "\ninput  [" << text << "]\noutput [" << written << ']');
            FAIL();
        }
    }
}

TEST_CASE("the hard cases round-trip exactly", "[format][roundtrip]") {
    const std::vector<std::string> cases{
        "",
        "\n",
        "\r\n",
        "no trailing newline",
        "\xEF\xBB\xBF[PROJECT]\nADX_VERSION=2\n",
        "[PROJECT]\r\nADX_VERSION=2\r\n",
        "trailing whitespace   \n",
        "\n\n\n",
        "[A]\n[B]\n",
        std::string("with") + std::string(1, '\0') + std::string("nul"),
    };
    for (const std::string& text : cases) {
        DiagnosticList diagnostics;
        const Document document = Document::parse(text, diagnostics);
        INFO("case [" << text << ']');
        CHECK(document.write() == text);
    }
}

namespace {

/// Mutates random bytes of `base`, then runs the *whole* load - document, v2 parser
/// or v1 shim, commands, residue - and the writer over the result.
///
/// Every input produces diagnostics, never an uncaught throw, never a crash, never a
/// hang. Catch2 turns an escaping exception into a failure, so reaching the end is
/// the assertion; the document round-trip is checked on the way as a bonus.
void fuzzLoad(const std::string& base, std::uint32_t seed, int cases) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<std::size_t> position(0, base.size() - 1);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<int> edits(1, 24);

    for (int i = 0; i < cases; ++i) {
        std::string mutated = base;
        const int count = edits(rng);
        for (int edit = 0; edit < count; ++edit) {
            mutated[position(rng)] = static_cast<char>(byte(rng));
        }
        DiagnosticList diagnostics;
        const Document document = Document::parse(mutated, diagnostics);
        REQUIRE(document.write() == mutated);

        adx::project::Project project;
        adx::project::CommandStack stack;
        adx::format::load(mutated, project, stack, diagnostics);
        (void)adx::format::write(project);
    }
}

} // namespace

TEST_CASE("parser_no_exceptions_on_fuzz", "[format][roundtrip]") {
    // Both grammars: suffocation.adx goes through the v1 shim, its upgrade through the
    // v2 parser. A few hundred here for the fast loop; the full hundred thousand are in
    // the [.slow] case below, which ctest runs by default.
    const std::string v1 = adx::tests::readFile(adx::tests::corpusPaths().back());
    REQUIRE_FALSE(v1.empty());
    adx::project::Project project;
    adx::project::CommandStack stack;
    DiagnosticList diagnostics;
    adx::format::load(v1, project, stack, diagnostics);
    const std::string v2 = adx::format::write(project);

    fuzzLoad(v1, 4242U, 200);
    fuzzLoad(v2, 4243U, 200);
}

TEST_CASE("parser_no_exceptions_on_fuzz 100k", "[format][roundtrip][.slow]") {
    // A full load of suffocation.adx is ~0.6 ms optimised and ~16 ms in Debug, so a
    // hundred thousand in Debug would take most of half an hour for no extra coverage:
    // the inputs are the same, only the speed differs. Debug runs a tenth, every
    // optimised configuration - RelWithDebInfo included, with its assertions live -
    // runs the lot.
#if defined(NDEBUG)
    constexpr int kCasesPerGrammar = 50000;
#else
    constexpr int kCasesPerGrammar = 5000;
#endif
    const std::string v1 = adx::tests::readFile(adx::tests::corpusPaths().back());
    adx::project::Project project;
    adx::project::CommandStack stack;
    DiagnosticList diagnostics;
    adx::format::load(v1, project, stack, diagnostics);
    const std::string v2 = adx::format::write(project);

    fuzzLoad(v1, 1U, kCasesPerGrammar);
    fuzzLoad(v2, 2U, kCasesPerGrammar);
}

TEST_CASE("document_preserves_unknown", "[format][roundtrip]") {
    // A file with a key and a section this build does not understand survives
    // load and save unchanged, and says so rather than staying silent.
    const std::string text = "[PROJECT]\n"
                             "ADX_VERSION=2\n"
                             "FUTURE_KEY=1\n"
                             "\n"
                             "[FUTURE_SECTION]\n"
                             "whatever it holds\n";

    DiagnosticList diagnostics;
    const Document document = Document::parse(text, diagnostics);
    CHECK(document.write() == text);

    // Nothing has claimed anything yet, so every data line is residue.
    const auto residue = document.residue();
    CHECK_FALSE(residue.empty());

    bool sawSection = false;
    for (const auto& block : residue.blocks) {
        if (block.wholeSection && block.sectionType == "FUTURE_SECTION") {
            sawSection = true;
            REQUIRE(block.lines.size() == 1);
            CHECK(block.lines.front().raw == "whatever it holds");
        }
    }
    CHECK(sawSection);
}
