// The file watcher (phase_7.md §4.4, §6): it sees a save however the editor makes it,
// and a burst of writes is one reload.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "engine/sync/FileWatcher.h"

using adx::sync::FileWatcher;

namespace {

std::filesystem::path scratchDirectory(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("adx_" + std::string(name) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
}

/// Polls like the app's timer does, for `duration`, counting reloads.
int reloadsWithin(FileWatcher& watcher, std::chrono::milliseconds duration) {
    int reloads = 0;
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
        reloads += watcher.poll() ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return reloads;
}

} // namespace

TEST_CASE("watcher_handles_rename_write", "[sync][watcher]") {
    // Most editors save by writing a temporary and renaming it over the file: a delete
    // and a create, or a rename - not a modification. One save is one reload.
    const auto dir = scratchDirectory("watch_rename");
    const auto file = dir / "song.adx";
    writeFile(file, "a");
    FileWatcher watcher(file, std::chrono::milliseconds(150));
    REQUIRE(watcher.watching());

    writeFile(dir / ".song.adx.tmp", "b");
    std::filesystem::rename(dir / ".song.adx.tmp", file);
    CHECK(reloadsWithin(watcher, std::chrono::milliseconds(800)) == 1);

    // Delete-then-create, as some editors do.
    std::filesystem::remove(file);
    writeFile(file, "c");
    CHECK(reloadsWithin(watcher, std::chrono::milliseconds(800)) == 1);

    // A sibling file is not this file.
    writeFile(dir / "other.adx", "x");
    CHECK(reloadsWithin(watcher, std::chrono::milliseconds(400)) == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("watcher_debounces", "[sync][watcher]") {
    // Fifty rapid writes are one burst: at most two reloads, however the burst lands.
    const auto dir = scratchDirectory("watch_burst");
    const auto file = dir / "song.adx";
    writeFile(file, "0");
    FileWatcher watcher(file, std::chrono::milliseconds(150));
    REQUIRE(watcher.watching());
    int reloads = 0;
    for (int i = 0; i < 50; ++i) {
        writeFile(file, std::to_string(i));
        reloads += watcher.poll() ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    reloads += reloadsWithin(watcher, std::chrono::milliseconds(800));
    CHECK(watcher.events() >= 1);
    CHECK(reloads >= 1);
    CHECK(reloads <= 2);
    std::filesystem::remove_all(dir);
}
