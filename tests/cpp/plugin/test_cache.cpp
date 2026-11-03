// The plugin cache (phase_9.md §4.2): what a scan found, kept on disk, invalidated by a
// file's mtime and size, failures kept as the blacklist.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "engine/plugin/PluginCache.h"
#include "engine/plugin/Scanner.h"
#include "tests/cpp/plugin/PluginFixtures.h"

namespace {

using namespace adx::plugin;
namespace fx = adx::tests::plugins;
namespace fs = std::filesystem;

} // namespace

TEST_CASE("scanner_cache_invalidation", "[plugin][scanner][cache]") {
    const fs::path dir = fx::freshDirectory("cache-invalidation");
    const fs::path copy = dir / "adx_test_plugin.clap";
    fs::copy_file(fx::clapPath(), copy);
    const std::vector<Candidate> candidates{{.format = PluginFormat::Clap, .path = copy.string()}};
    ScanOptions options;
    options.executable = fx::hostExe();
    PluginCache cache;

    CHECK(scanAll(candidates, cache, options) == 1);
    const ScanResult* entry = cache.find(copy.string());
    REQUIRE(entry != nullptr);
    CHECK(entry->ok);
    CHECK(entry->plugins.size() == 2);

    // Untouched: nothing to do.
    CHECK(scanAll(candidates, cache, options) == 0);

    // Touched: scanned again.
    fs::last_write_time(copy, fs::last_write_time(copy) + std::chrono::seconds(5));
    CHECK(scanAll(candidates, cache, options) == 1);
    CHECK(scanAll(candidates, cache, options) == 0);

    // Forced: scanned whatever the cache says.
    options.force = true;
    CHECK(scanAll(candidates, cache, options) == 1);
}

TEST_CASE("plugin_cache_saves_and_reloads", "[plugin][cache]") {
    const fs::path dir = fx::freshDirectory("cache-roundtrip");
    ScanOptions options;
    options.executable = fx::hostExe();
    PluginCache cache;
    cache.setPath((dir / "nested" / "cache.json").string());
    static_cast<void>(scanAll({{.format = PluginFormat::Clap, .path = fx::clapPath()},
                               {.format = PluginFormat::Vst3, .path = fx::vst3Path()}},
                              cache, options));
    ScanResult failed;
    failed.path = "C:/nowhere/broken.clap";
    failed.ok = false;
    failed.error = "the scanner crashed (exit code 0xC0000005)";
    cache.put(failed);
    std::string error;
    REQUIRE(cache.save(error));

    PluginCache back;
    REQUIRE(back.load(cache.path(), error));
    CHECK(back.entries() == cache.entries());
    CHECK(back.plugins().size() == 4);
    REQUIRE(back.blacklist().size() == 1);
    CHECK(back.blacklist().front()->error.find("crashed") != std::string::npos);
    CHECK(back.findPlugin(PluginFormat::Clap, "com.adx.test.synth") != nullptr);
    CHECK(back.findPlugin(PluginFormat::Vst3, "54584441464558460000000100000001") != nullptr);

    // A missing cache is an empty one; a corrupt one is reported and empty.
    PluginCache missing;
    CHECK(missing.load((dir / "none.json").string(), error));
    CHECK(missing.entries().empty());
    {
        std::ofstream file(dir / "bad.json", std::ios::binary);
        file << R"({"version":1,"entries":[{)";
    }
    PluginCache corrupt;
    CHECK_FALSE(corrupt.load((dir / "bad.json").string(), error));
    CHECK(corrupt.entries().empty());
}
