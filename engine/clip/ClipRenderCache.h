// adx-thread: main
//
// The buffer an audio clip actually plays, content-addressed: the decoded file with its
// destructive edits, its reverse, conversion to the project's rate, and its stretch,
// pitch and warp all applied (phase_8.md §3.2-§3.3).
//
// One worker-side pipeline, so every clip property that changes what is heard changes
// one key - and the playlist's waveform asks for its peaks by the same key
// (ClipPeakCache), so the display and the audio cannot disagree about what is playing
// (warp_cache_key).
//
// Like the sample pool: request() returns at once, workers render, the audio thread
// reads the entry's handle and plays silence (or the clip unstretched) until it is
// ready; an offline render calls waitAll() first. Entries live while something holds
// them - a snapshot's pins - and are shared between every clip with the same key.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/format/audio/SampleBuffer.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/project/Playlist.h"

namespace adx::clip {

struct ClipRenderKey {
    /// Resolved, on disk.
    std::filesystem::path path;
    std::vector<project::ClipEdit> edits;
    bool reverse{false};
    double stretch{1.0};
    double pitchSemitones{0.0};
    /// The rate the result is at: the project's.
    std::uint32_t sampleRate{48000};
    /// Warp: (source seconds, output seconds) pairs. When present, `stretch` is ignored -
    /// the markers say where everything lands.
    std::vector<std::pair<double, double>> warp;

    /// Canonical text of every field: the map key, and the peak cache's.
    [[nodiscard]] std::string text() const;
};

class ClipRenderEntry {
public:
    [[nodiscard]] const format::SampleHandle& handle() const noexcept {
        return m_handle;
    }
    /// Null until ready.
    [[nodiscard]] std::shared_ptr<const format::SampleBuffer> buffer() const;
    [[nodiscard]] const std::string& error() const noexcept {
        return m_error;
    }
    /// The decoded file this renders from, pinned for as long as this entry lives.
    [[nodiscard]] const std::shared_ptr<const format::SampleEntry>& source() const noexcept {
        return m_source;
    }

private:
    friend class ClipRenderCache;
    format::SampleHandle m_handle;
    std::shared_ptr<const format::SampleBuffer> m_buffer;
    std::shared_ptr<const format::SampleEntry> m_source;
    std::string m_error;
    std::string m_key;
};

class ClipRenderCache {
public:
    explicit ClipRenderCache(unsigned workers = 2);
    ~ClipRenderCache();
    ClipRenderCache(const ClipRenderCache&) = delete;
    ClipRenderCache& operator=(const ClipRenderCache&) = delete;
    ClipRenderCache(ClipRenderCache&&) = delete;
    ClipRenderCache& operator=(ClipRenderCache&&) = delete;

    static ClipRenderCache& global();

    /// The entry for `key`, its source requested from `pool` and its render queued
    /// when it is new.
    [[nodiscard]] std::shared_ptr<const ClipRenderEntry> request(const ClipRenderKey& key,
                                                                 format::SamplePool& pool);

    /// Blocks until every queued render has finished, waiting for their sources too.
    void waitAll();

    /// Renders done since construction, for tests: a shared entry does not count.
    [[nodiscard]] std::uint64_t rendersDone() const;

private:
    struct Job {
        std::weak_ptr<ClipRenderEntry> entry;
        ClipRenderKey key;
    };
    void work(const std::stop_token& stop);
    static void render(ClipRenderEntry& entry, const ClipRenderKey& key);

    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::condition_variable m_idle;
    std::deque<Job> m_queue;
    std::size_t m_running{0};
    std::uint64_t m_done{0};
    std::unordered_map<std::string, std::weak_ptr<ClipRenderEntry>> m_entries;
    std::vector<std::jthread> m_workers;
};

/// The whole pipeline on one buffer, synchronously: what a worker runs, exposed for
/// tests and for freeze.
[[nodiscard]] format::SampleBuffer renderClip(const format::SampleBuffer& source,
                                              const ClipRenderKey& key);

} // namespace adx::clip
