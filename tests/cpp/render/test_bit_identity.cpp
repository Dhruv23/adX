// THE PHASE GATE (phase_3.md §7): offline render == realtime capture, bit for bit.
//
// A synthetic 200-channel / 100k-note project, played on a NullBackend - a real audio
// thread on a real clock - and captured, then rendered offline through the same
// engine. Identical RenderHash at block sizes 64, 256 and 1024. The three sizes are
// the point: a block-size-dependent bug hides at any one of them (§10 risks).
//
// Also here: the golden corpus, which is what turns "bit-identical" from a property
// of one build into a property of the project's history.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

#include "engine/format/adx/Parser.h"
#include "engine/render/RenderHash.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/render/GoldenHashes.h"
#include "tests/cpp/render/RenderEvidence.h"
#include "tests/cpp/render/RenderFixtures.h"

TEST_CASE("render_bit_identical_200x100k", "[render][gate][.slow]") {
    const adx::tests::RenderEvidence& evidence = adx::tests::loadEvidence();
    INFO(evidence.description);
    REQUIRE(evidence.runs.size() == 3);
    for (const adx::tests::RenderRun& run : evidence.runs) {
        INFO("block " << run.blockFrames << ": offline " << run.offline.hex() << ", realtime "
                      << run.realtime.hex());
        CHECK(run.offlinePeak > 0.0F);
        CHECK(run.offline == run.realtime);
    }
}

namespace {

/// Renders one corpus file the way the corpus always has: 48 kHz, 256-frame blocks,
/// from the start to the end of the arrangement plus a quarter second of tail, capped
/// at ten seconds so an example project cannot make the suite slow.
std::string renderHash(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(text, project, stack, diagnostics);
    REQUIRE_FALSE(diagnostics.hasErrors());

    adx::render::OfflineRenderOptions options;
    options.tailSeconds = 0.25;
    const std::int64_t natural =
        project.tempo.toSamples(project.contentLength(), options.sampleRate).value + 12000;
    options.frames =
        static_cast<std::uint64_t>(std::min<std::int64_t>(natural, std::int64_t{48000} * 10));
    adx::render::RenderStats stats;
    static_cast<void>(adx::render::renderOffline(project, options, stats));
    REQUIRE(stats.error.empty());
    CHECK(stats.peak > 0.0F);
    return stats.hash.hex();
}

} // namespace

TEST_CASE("golden_corpus_stable", "[render][golden]") {
    // Every file in tests/golden/ and every example in docs/examples/ renders to the
    // hash committed in tests/golden/hashes.txt. A change here is either a bug or a
    // deliberate change to what adX sounds like; in the second case, regenerate with
    // ADX_UPDATE_GOLDEN=1 and say why in the commit.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(adx::tests::goldenDirectory())) {
        if (entry.path().extension() == ".adx") {
            files.push_back(entry.path());
        }
    }
    for (const auto& example : adx::tests::corpusPaths()) {
        files.push_back(example);
    }
    std::ranges::sort(files);
    REQUIRE(files.size() >= 5);

    std::map<std::string, std::string> actual;
    for (const auto& file : files) {
        const std::string name =
            std::filesystem::relative(file, adx::tests::repoRoot()).generic_string();
        actual[name] = renderHash(file);
    }
    adx::tests::checkGolden(
        adx::tests::goldenDirectory() / "hashes.txt",
        "# Render hashes for the golden corpus: 48 kHz, 256-frame blocks, stereo,\n"
        "# FNV-1a 128 over the raw float bytes. Regenerate with ADX_UPDATE_GOLDEN=1,\n"
        "# and only on purpose.\n",
        actual);
}
