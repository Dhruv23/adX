// adx-thread: main
//
// Tells the main thread when a file changed on disk - the archive's 500 ms stat()
// poll (main.cpp::CheckForHotReload) with the behaviour kept and the mechanism
// replaced (phase_7.md §5).
//
// Editors do not modify files in place. Most write a temporary and rename it over the
// target, which arrives as a delete and a create - or as a rename - rather than as a
// modification; some write twice; a save can be several events. So this watcher does
// not trust the kind of event at all: anything that names the file marks it changed,
// and poll() reports a change only after kWatcherDebounceMs of quiet, once, however
// many events made it. The caller then re-reads the whole file.
//
// On Windows a worker thread blocks in ReadDirectoryChangesW on the file's directory.
// Elsewhere (adX builds on Windows; this keeps the class portable) a worker polls the
// file's size and modification time every 50 ms.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>

namespace adx::sync {

class FileWatcher {
public:
    explicit FileWatcher(std::filesystem::path file,
                         std::chrono::milliseconds debounce = std::chrono::milliseconds{150});
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;
    FileWatcher(FileWatcher&&) = delete;
    FileWatcher& operator=(FileWatcher&&) = delete;

    /// Main thread. True once for each burst of changes, when the burst has been quiet
    /// for the debounce window. Cheap: two atomic loads when nothing happened.
    [[nodiscard]] bool poll();

    /// False when the directory could not be watched (it does not exist, say).
    [[nodiscard]] bool watching() const noexcept {
        return m_watching.load(std::memory_order_acquire);
    }

    /// Raw events that named the file, for tests.
    [[nodiscard]] std::uint64_t events() const noexcept {
        return m_generation.load(std::memory_order_acquire);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return m_file;
    }

private:
    struct Platform;

    void run();
    void touched() noexcept;

    std::filesystem::path m_file;
    std::chrono::milliseconds m_debounce;
    std::unique_ptr<Platform> m_platform;
    std::thread m_thread;
    std::atomic<bool> m_watching{false};
    std::atomic<bool> m_stop{false};
    /// Bumped by the worker on every event naming the file.
    std::atomic<std::uint64_t> m_generation{0};
    /// steady_clock nanoseconds of the latest such event.
    std::atomic<std::int64_t> m_lastEventNs{0};
    /// The generation poll() last reported. Main thread only.
    std::uint64_t m_reported{0};
};

} // namespace adx::sync
