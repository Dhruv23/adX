// Out-of-process scanning (phase_9.md §4.2, §5): a plugin that crashes, hangs or opens
// a dialog while being scanned is recorded as failed, and the scan goes on.
//
// adx_test_plugin's load-time misbehaviour comes from ADX_TEST_PLUGIN, which the scanner
// child inherits. The hang test uses a short deadline so it runs in seconds; the [.slow]
// one uses the real 30 s deadline against a 60 s hang, as §5 states it.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "engine/plugin/PluginCache.h"
#include "engine/plugin/Scanner.h"
#include "tests/cpp/plugin/PluginFixtures.h"

namespace {

using namespace adx::plugin;
namespace fx = adx::tests::plugins;
namespace fs = std::filesystem;

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

TEST_CASE("scanner_finds_both_formats", "[plugin][scanner]") {
    const fs::path dir = fx::freshDirectory("scanner-find");
    fs::create_directories(dir / "sub" / "Bundle.vst3" / "Contents" / "x86_64-win");
    fs::copy_file(fx::clapPath(), dir / "sub" / "One.clap");
    fs::copy_file(fx::vst3Path(), dir / "Two.vst3");
    const std::vector<Candidate> found = findPlugins({dir.string()});
    REQUIRE(found.size() == 3);
    // Two .vst3 (a file and a bundle directory, not descended into) and one .clap.
    CHECK(std::ranges::count(found, PluginFormat::Vst3, &Candidate::format) == 2);
    CHECK(std::ranges::count(found, PluginFormat::Clap, &Candidate::format) == 1);

    ScanOptions options;
    options.executable = fx::hostExe();
    const ScanResult clap =
        scanFile({.format = PluginFormat::Clap, .path = fx::clapPath()}, options);
    REQUIRE(clap.ok);
    REQUIRE(clap.plugins.size() == 2);
    CHECK(clap.plugins[0].id.name == "adX Test Effect");
    CHECK_FALSE(clap.plugins[0].instrument);
    CHECK(clap.plugins[1].instrument);
    const ScanResult vst3 =
        scanFile({.format = PluginFormat::Vst3, .path = fx::vst3Path()}, options);
    REQUIRE(vst3.ok);
    REQUIRE(vst3.plugins.size() == 2);
    CHECK(vst3.plugins[1].instrument);
    // A file that is no plugin at all fails with a reason, not a crash.
    const ScanResult notPlugin =
        scanFile({.format = PluginFormat::Clap, .path = fx::hostExe()}, options);
    CHECK_FALSE(notPlugin.ok);
    CHECK_FALSE(notPlugin.error.empty());
}

TEST_CASE("scanner_survives_crash_on_load", "[plugin][scanner]") {
    const fs::path dir = fx::freshDirectory("scanner-crash");
    const fs::path bad = dir / "crashes.clap";
    fs::copy_file(fx::clapPath(), bad);
    ScanOptions options;
    options.executable = fx::hostExe();
    PluginCache cache;
    {
        const fx::ScopedEnv env("ADX_TEST_PLUGIN", "crash");
        CHECK(scanAll({{PluginFormat::Clap, bad.string()}}, cache, options) == 1);
    }
    // And a good one after it, unaffected.
    CHECK(scanAll({{PluginFormat::Clap, fx::clapPath()}}, cache, options) == 1);
    const ScanResult* crashed = cache.find(bad.string());
    REQUIRE(crashed != nullptr);
    CHECK_FALSE(crashed->ok);
    CHECK(crashed->error.find("crashed") != std::string::npos);
    CHECK(cache.blacklist().size() == 1);
    REQUIRE(cache.find(fx::clapPath()) != nullptr);
    CHECK(cache.find(fx::clapPath())->ok);
    // Blacklisted: not scanned again until it changes or the user forces it.
    CHECK(scanAll({{PluginFormat::Clap, bad.string()}}, cache, options) == 0);
}

TEST_CASE("scanner_survives_hang", "[plugin][scanner]") {
    ScanOptions options;
    options.executable = fx::hostExe();
    options.deadlineMillis = 2000;
    const fx::ScopedEnv env("ADX_TEST_PLUGIN", "hang:60");
    const auto start = std::chrono::steady_clock::now();
    const ScanResult result =
        scanFile({.format = PluginFormat::Clap, .path = fx::clapPath()}, options);
    const double took = secondsSince(start);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("timed out") != std::string::npos);
    CHECK(took >= 1.9);
    CHECK(took < 10.0);
}

TEST_CASE("scanner_survives_dialog_on_scan", "[plugin][scanner]") {
    ScanOptions options;
    options.executable = fx::hostExe();
    options.deadlineMillis = 2000;
    const fx::ScopedEnv env("ADX_TEST_PLUGIN", "dialog");
    const ScanResult result =
        scanFile({.format = PluginFormat::Vst3, .path = fx::vst3Path()}, options);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("timed out") != std::string::npos);
}

TEST_CASE("scanner_survives_hang_at_the_real_deadline", "[plugin][scanner][.slow]") {
    ScanOptions options;
    options.executable = fx::hostExe(); // the default 30 s deadline
    const fx::ScopedEnv env("ADX_TEST_PLUGIN", "hang:60");
    const auto start = std::chrono::steady_clock::now();
    const ScanResult result =
        scanFile({.format = PluginFormat::Clap, .path = fx::clapPath()}, options);
    const double took = secondsSince(start);
    CHECK_FALSE(result.ok);
    CHECK(took >= 29.5);
    CHECK(took < 40.0);
}
