// adx-thread: main
//
// The sample pool: every decoded sample the engine is using, shared, refcounted and
// decoded off the calling thread (phase_4.md §4.11).
//
//   dedup       one entry per resolved path, so ten zones or clips on one file are one
//               entry; and one buffer per file *content*, so the same recording saved
//               under two names decodes once. Iteration one decoded a copy per CLIP.
//   refcount    an entry lives while anything holds its shared_ptr - a sampler node's
//               pins, a test. Nodes are destroyed by the Reaper on the main thread, so
//               a buffer is never freed on the audio thread.
//   async       request() returns at once; worker threads decode. A node plays silence
//               until its handle is ready() and never waits. An offline render calls
//               waitAll() first, so it renders the samples rather than their absence.
//   paths       stored relative to the project file (Resources.h); resolved against
//               the project directory, then against each search path, by relative path
//               and then by file name - so a project whose samples moved alongside it,
//               or into a library folder, still finds them. Collect-and-save is Phase 8.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine/format/audio/SampleBuffer.h"
#include "engine/format/audio/SampleView.h"

namespace adx::project {
struct Resources;
}

namespace adx::format {

class SamplePool;

/// One requested file. The handle is what the audio thread reads.
class SampleEntry {
public:
    SampleEntry(SamplePool& pool, std::filesystem::path path);
    ~SampleEntry();
    SampleEntry(const SampleEntry&) = delete;
    SampleEntry& operator=(const SampleEntry&) = delete;
    SampleEntry(SampleEntry&&) = delete;
    SampleEntry& operator=(SampleEntry&&) = delete;

    [[nodiscard]] const SampleHandle& handle() const noexcept {
        return m_handle;
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return m_path;
    }
    /// Why decoding failed; empty otherwise. Valid once the handle is not Loading.
    [[nodiscard]] const std::string& error() const noexcept {
        return m_error;
    }
    /// The decoded buffer, shared with every entry whose file has the same content.
    /// Null until ready.
    [[nodiscard]] std::shared_ptr<const SampleBuffer> buffer() const;

private:
    friend class SamplePool;
    SamplePool* m_pool;
    std::filesystem::path m_path;
    /// The pool's map key, kept so the destructor can find the entry without
    /// allocating.
    std::string m_key;
    SampleHandle m_handle;
    std::string m_error;
    std::shared_ptr<const SampleBuffer> m_buffer;
};

struct SamplePoolStats {
    std::uint64_t requests{0};
    /// Files actually decoded. Fewer than requests when entries or contents are shared.
    std::uint64_t decodes{0};
    /// Buffers freed: each exactly once, when its last entry goes.
    std::uint64_t buffersFreed{0};
    std::uint64_t liveEntries{0};
};

class SamplePool {
public:
    explicit SamplePool(unsigned workers = 2);
    ~SamplePool();
    SamplePool(const SamplePool&) = delete;
    SamplePool& operator=(const SamplePool&) = delete;
    SamplePool(SamplePool&&) = delete;
    SamplePool& operator=(SamplePool&&) = delete;

    /// The process-wide pool every graph uses unless told otherwise.
    static SamplePool& global();

    /// The entry for `path` (already resolved), queued for decoding when new.
    [[nodiscard]] std::shared_ptr<const SampleEntry> request(const std::filesystem::path& path);

    /// Where `relative` (as written in the project) is on disk: the project directory,
    /// then each search path by relative path, then each search path by file name.
    /// The first candidate when none exists, so the failure names a real place.
    [[nodiscard]] std::filesystem::path resolve(const project::Resources& resources,
                                                std::string_view relative) const;
    void addSearchPath(std::filesystem::path directory);

    /// Blocks until every queued decode has finished. Offline render and tests.
    void waitAll();

    [[nodiscard]] SamplePoolStats stats() const;

private:
    friend class SampleEntry;
    void work();
    void decode(const std::shared_ptr<SampleEntry>& entry);
    void entryDestroyed(SampleEntry& entry) noexcept;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::deque<std::weak_ptr<SampleEntry>> m_queue;
    std::size_t m_inFlight{0};
    bool m_stopping{false};
    std::unordered_map<std::string, std::weak_ptr<SampleEntry>> m_byPath;
    std::unordered_map<std::uint64_t, std::weak_ptr<const SampleBuffer>> m_byContent;
    std::vector<std::filesystem::path> m_searchPaths;
    SamplePoolStats m_stats;
    std::shared_ptr<std::atomic<std::uint64_t>> m_freed =
        std::make_shared<std::atomic<std::uint64_t>>(0);
    std::vector<std::thread> m_workers;
};

} // namespace adx::format
