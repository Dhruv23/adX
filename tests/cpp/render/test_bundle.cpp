// Collect-and-save (phase_8.md §3.7, §5), and the example corpus that depends on it
// being self-contained.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "engine/format/adx/Parser.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/render/Bundle.h"
#include "engine/render/OfflineRender.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/clip/ClipFixtures.h"

namespace fs = std::filesystem;
using adx::tests::ClipDir;

namespace {

constexpr std::uint32_t kRate = 48000;

std::vector<float> renderFile(const fs::path& path) {
    adx::project::Project project;
    adx::project::CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(adx::tests::readFile(path), project, stack, diagnostics,
                      adx::format::ParseOptions{.baseDirectory = path.parent_path().string()});
    adx::render::OfflineRenderOptions options;
    options.frames = static_cast<std::size_t>(kRate) * 2;
    adx::render::RenderStats stats;
    return adx::render::renderOffline(project, options, stats);
}

void writeText(const fs::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
}

/// A project with its samples in three different places - beside it, in a library
/// folder found only by SAMPLE_PATHS, and at an absolute path - and the formatting of a
/// person who writes by hand: comments, alignment, CRLF.
struct Scattered {
    ClipDir root;
    fs::path project;
    std::string text;

    Scattered() {
        fs::create_directories(root.path() / "song" / "audio");
        fs::create_directories(root.path() / "library");
        fs::create_directories(root.path() / "elsewhere");
        static_cast<void>(
            root.write("song/audio/vox.wav", adx::tests::stereoNoise(kRate, 1), kRate));
        static_cast<void>(
            root.write("library/pad.wav", adx::tests::stereoNoise(kRate / 2, 2), kRate));
        const fs::path absolute =
            root.write("elsewhere/hit.wav", adx::tests::stereoSine(kRate / 4, kRate, 220.0), kRate);
        project = root.path() / "song" / "song.adx";
        text = "[PROJECT]\r\nADX_VERSION=2\r\nSAMPLE_PATHS=\"../library\"\r\n\r\n"
               "# tempo first, the way I like it\r\n[TEMPO]\r\n0:0:0 120\r\n\r\n"
               "[PLAYLIST]\r\nTRACK 1   name=\"Vocals\"\r\n"
               "  AUDIO \"audio/vox.wav\"    0:0:0    # the take\r\n"
               "  AUDIO \"pad.wav\" 0:1:0 fadein=0:0:960\r\n"
               "TRACK 2\r\n  AUDIO \"" +
               absolute.generic_string() +
               "\" 0:2:0\r\n  AUDIO \"audio/vox.wav\" 1:0:0\r\n\r\n"
               "[MIXER]\r\nINSERT 1 name=\"Master\"\r\n";
        writeText(project, text);
    }
};

} // namespace

TEST_CASE("bundle_collects_and_rewrites", "[render][bundle]") {
    Scattered const scattered;
    const std::vector<float> before = renderFile(scattered.project);

    const fs::path bundle = scattered.root.path() / "out" / "song bundle";
    const adx::render::BundleResult result = adx::render::bundleProject(adx::render::BundleOptions{
        .project = scattered.project, .destination = bundle, .zip = true, .library = {}});
    INFO(result.error);
    REQUIRE(result.ok);
    CHECK(result.missing.empty());
    CHECK(result.filesCopied == 3);
    for (const char* name : {"vox.wav", "pad.wav", "hit.wav"}) {
        CHECK(fs::is_regular_file(bundle / "samples" / name));
    }
    CHECK(fs::is_regular_file(result.zipOut));
    const std::string written = adx::tests::readFile(result.projectOut);
    CHECK(written.find("\"samples/vox.wav\"") != std::string::npos);
    CHECK(written.find("\"samples/hit.wav\"") != std::string::npos);
    CHECK(written.find("elsewhere") == std::string::npos);

    // The originals gone, the bundle still plays the same.
    fs::remove_all(scattered.root.path() / "song");
    fs::remove_all(scattered.root.path() / "library");
    fs::remove_all(scattered.root.path() / "elsewhere");
    REQUIRE(renderFile(result.projectOut) == before);
}

TEST_CASE("bundle_preserves_formatting", "[render][bundle]") {
    Scattered scattered;
    const fs::path bundle = scattered.root.path() / "bundle";
    const adx::render::BundleResult result = adx::render::bundleProject(adx::render::BundleOptions{
        .project = scattered.project, .destination = bundle, .zip = false, .library = {}});
    REQUIRE(result.ok);
    const std::string written = adx::tests::readFile(result.projectOut);
    // Putting the old paths back gives the original file, byte for byte: nothing but
    // the paths was touched.
    const std::string restored = adx::render::rewritePaths(
        written,
        [&result](std::string_view path, adx::render::PathKind) -> std::optional<std::string> {
            for (const auto& [before, after] : result.rewritten) {
                if (after == path) {
                    return before;
                }
            }
            return std::nullopt;
        });
    REQUIRE(restored == scattered.text);
}

TEST_CASE("bundle_reports_missing_files", "[render][bundle]") {
    ClipDir const dir;
    const fs::path project = dir.path() / "lost.adx";
    writeText(project, "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[PLAYLIST]\nTRACK 1\n"
                       "  AUDIO \"gone.wav\" 0:0:0\n\n[MIXER]\nINSERT 1 name=\"Master\"\n");
    const adx::render::BundleResult result = adx::render::bundleProject(adx::render::BundleOptions{
        .project = project, .destination = dir.path() / "b", .zip = false, .library = {}});
    REQUIRE(result.ok);
    REQUIRE(result.missing == std::vector<std::string>{"gone.wav"});
    CHECK(adx::tests::readFile(result.projectOut) == adx::tests::readFile(project));
}

TEST_CASE("examples_are_self_contained", "[render][bundle][corpus]") {
    // Every sample the example corpus names is found under docs/examples, and nothing
    // reaches into _archive/ (phase_8.md §6).
    const fs::path examples = adx::tests::repoRoot() / "docs" / "examples";
    for (const fs::path& path : adx::tests::corpusPaths()) {
        INFO(path.filename().string());
        adx::project::Project project;
        adx::project::CommandStack stack;
        adx::format::DiagnosticList diagnostics;
        adx::format::load(adx::tests::readFile(path), project, stack, diagnostics,
                          adx::format::ParseOptions{.baseDirectory = path.parent_path().string()});
        for (const adx::project::SampleRef& sample : project.resources.samples) {
            INFO(sample.path);
            const fs::path resolved =
                adx::format::resolveSamplePath(project.resources, {}, sample.path);
            REQUIRE(adx::format::sampleExists(resolved));
            const std::string where = fs::weakly_canonical(resolved).generic_string();
            CHECK(where.find("/_archive/") == std::string::npos);
            CHECK(where.starts_with(fs::weakly_canonical(examples).generic_string()));
        }
    }
}
