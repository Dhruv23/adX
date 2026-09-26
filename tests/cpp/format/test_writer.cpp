// The canonical writer.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Value.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "tests/cpp/Corpus.h"

using adx::format::DiagnosticList;
using adx::project::CommandStack;
using adx::project::Project;

namespace {

/// Loads `text` and writes the project back out in canonical form.
[[nodiscard]] std::string format(const std::string& text, DiagnosticList& diagnostics) {
    Project project;
    CommandStack stack;
    adx::format::load(text, project, stack, diagnostics);
    return adx::format::write(project);
}

[[nodiscard]] std::string format(const std::string& text) {
    DiagnosticList diagnostics;
    return format(text, diagnostics);
}

} // namespace

TEST_CASE("writer_canonical_idempotent corpus", "[format][writer]") {
    // fmt(fmt(x)) == fmt(x). The first pass normalises; every pass after it is a
    // no-op, which is what makes `adx fmt --check` meaningful in a pre-commit hook.
    for (const auto& path : adx::tests::corpusPaths()) {
        INFO(path.string());
        const std::string once = format(adx::tests::readFile(path));
        const std::string twice = format(once);
        CHECK(once == twice);
    }
}

TEST_CASE("writer_canonical_idempotent generated", "[format][writer]") {
    const std::string source =
        "[PROJECT]\nADX_VERSION=2\nTITLE=\"x\"\n"
        "\n[CHANNEL A]\nPARAM drive=0.1\n"
        "\n[PATTERN P]\nLENGTH=2:0:0\nNOTES A\n  C4 0:0:0 0:1:0 96 pan=-0.5\n"
        "AUTOMATION channel.A.drive\n  0:0:0 0.1 exponential(0.5)\n  1:0:0 0.9\n"
        "\n[PLAYLIST]\nTRACK 1 name=\"L\"\n  PATTERN P 0:0:0\n"
        "\n[MIXER]\nINSERT 1 name=\"Master\"\n"
        "\n[MARKERS]\n0:0:0 \"Start\"\n";

    const std::string once = format(source);
    CHECK(format(once) == once);
    CHECK(format(format(once)) == once);
}

TEST_CASE("a canonical file survives load and save unchanged", "[format][writer]") {
    // Rule 2 in its strictest form: for a file that is already canonical, parse then
    // write is byte-identical.
    const std::string canonical = format(adx::tests::readFile(adx::tests::corpusPaths().back()));
    DiagnosticList diagnostics;
    const std::string again = format(canonical, diagnostics);
    CHECK(again == canonical);
    CHECK_FALSE(diagnostics.hasErrors());
}

TEST_CASE("writer_float_shortest", "[format][writer]") {
    // std::to_chars' shortest round-trip form: `0.1` stays `0.1` and never becomes
    // `0.10000000149011612`, which is what makes a diff readable.
    CHECK(adx::format::formatDouble(0.1) == "0.1");
    CHECK(adx::format::formatFloat(0.1F) == "0.1");
    CHECK(adx::format::formatDouble(440.0) == "440");
    CHECK(adx::format::formatDouble(-0.25) == "-0.25");
    CHECK(adx::format::formatFloat(1.0F / 3.0F) == "0.33333334");

    const std::string text =
        "[PROJECT]\nADX_VERSION=2\nTUNING=440.1\n\n[CHANNEL A]\nPARAM drive=0.1\n";
    const std::string written = format(text);
    CHECK(written.find("TUNING=440.1") != std::string::npos);
    CHECK(written.find("PARAM drive=0.1") != std::string::npos);
}

TEST_CASE("unknown keys and sections come back out", "[format][writer]") {
    const std::string text =
        "[PROJECT]\nADX_VERSION=2\nFUTURE_KEY=1\n\n[FUTURE_SECTION]\nsomething\n";
    const std::string written = format(text);
    INFO(written);
    CHECK(written.find("FUTURE_KEY=1") != std::string::npos);
    CHECK(written.find("[FUTURE_SECTION]") != std::string::npos);
    CHECK(written.find("something") != std::string::npos);
    // And it stays stable from there.
    CHECK(format(written) == written);
}

TEST_CASE("notes are written in (start, pitch) order", "[format][writer]") {
    const std::string text = "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL A]\nVOLUME=1\n"
                             "\n[PATTERN P]\nLENGTH=4:0:0\nNOTES A\n"
                             "  G4 0:2:0 0:1:0 96\n  C4 0:1:0 0:1:0 96\n  E4 0:1:0 0:1:0 96\n";
    const std::string written = format(text);
    const std::size_t c4 = written.find("C4 0:1:0");
    const std::size_t e4 = written.find("E4 0:1:0");
    const std::size_t g4 = written.find("G4 0:2:0");
    REQUIRE(c4 != std::string::npos);
    REQUIRE(e4 != std::string::npos);
    REQUIRE(g4 != std::string::npos);
    CHECK(c4 < e4);
    CHECK(e4 < g4);
}

TEST_CASE("defaults are omitted so a note is one line", "[format][writer]") {
    const std::string text = "[PROJECT]\nADX_VERSION=2\n\n[CHANNEL A]\nVOLUME=1\n"
                             "\n[PATTERN P]\nLENGTH=4:0:0\nNOTES A\n  C4 0:0:0 0:1:0 96\n";
    const std::string written = format(text);
    INFO(written);
    CHECK(written.find("C4 0:0:0 0:1:0 96\n") != std::string::npos);
    CHECK(written.find("pan=") == std::string::npos);
    CHECK(written.find("rel=") == std::string::npos);
}

TEST_CASE("line endings are a choice, and the default is LF", "[format][writer]") {
    Project project;
    CommandStack stack;
    DiagnosticList diagnostics;
    adx::format::load("[PROJECT]\nADX_VERSION=2\n", project, stack, diagnostics);

    adx::format::WriteOptions crlf;
    crlf.lineEnding = "\r\n";
    const std::string written = adx::format::write(project, crlf);
    CHECK(written.find("\r\n") != std::string::npos);
    CHECK(adx::format::write(project).find('\r') == std::string::npos);
}
