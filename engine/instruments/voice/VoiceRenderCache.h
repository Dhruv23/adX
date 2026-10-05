// adx-thread: main
//
// Rendered notes, content-addressed (phase_4.md §4.13): VoiceRenderKey -> buffer.
//
// request() returns at once with an entry whose clip is not yet ready, and queues the
// render on a worker thread unless an entry for the key already exists - the same note
// in two places, or an unchanged note after an edit elsewhere, renders once. Entries
// live as long as anything holds them (a Voice node's pins) or the cache does; the cache
// keeps every entry it has made for the life of the process, which is what lets an
// undo find its old renders. waitAll() is what an export and a golden render call, so
// an offline render is the same however fast the workers were.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "engine/instruments/voice/UtauResampler.h"
#include "engine/instruments/voice/VoiceClip.h"

namespace adx::instruments {

class VoiceRenderCache {
public:
    struct Entry {
        std::uint64_t key{0};
        VoiceClip clip;
        std::vector<float> samples;
        /// False when the render failed (no analysis): the clip stays empty but ready.
        bool ok{false};
    };

    explicit VoiceRenderCache(std::size_t workers = 2);
    ~VoiceRenderCache();
    VoiceRenderCache(const VoiceRenderCache&) = delete;
    VoiceRenderCache& operator=(const VoiceRenderCache&) = delete;
    VoiceRenderCache(VoiceRenderCache&&) = delete;
    VoiceRenderCache& operator=(VoiceRenderCache&&) = delete;

    /// The process-wide cache the instrument factory uses.
    static VoiceRenderCache& global();

    [[nodiscard]] std::shared_ptr<const Entry> request(const NoteRenderRequest& request);

    /// Blocks until every queued render has finished.
    void waitAll();

    /// Renders done since construction, for tests: a cache hit does not count.
    [[nodiscard]] std::size_t rendersDone() const;

private:
    struct Job {
        std::shared_ptr<Entry> entry;
        NoteRenderRequest request;
    };
    void work(const std::stop_token& stop);

    mutable std::mutex m_mutex;
    std::condition_variable_any m_wake;
    std::condition_variable m_idle;
    std::deque<Job> m_queue;
    std::size_t m_running{0};
    std::size_t m_done{0};
    std::unordered_map<std::uint64_t, std::shared_ptr<Entry>> m_entries;
    std::vector<std::jthread> m_workers;
};

} // namespace adx::instruments
