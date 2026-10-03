// adx-thread: main
#include "engine/format/audio/SamplePool.h"

#include <algorithm>
#include <utility>

#include "engine/format/audio/AudioFileLoader.h"
#include "engine/project/Resources.h"

namespace adx::format {

SampleEntry::SampleEntry(SamplePool& pool, std::filesystem::path path)
    : m_pool(&pool), m_path(std::move(path)) {}

SampleEntry::~SampleEntry() {
    m_pool->entryDestroyed(*this);
}

std::shared_ptr<const SampleBuffer> SampleEntry::buffer() const {
    return m_handle.ready() ? m_buffer : nullptr;
}

SamplePool::SamplePool(unsigned workers) {
    workers = std::max(1U, workers);
    m_workers.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        m_workers.emplace_back([this] { work(); });
    }
}

SamplePool::~SamplePool() {
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();
    for (std::thread& worker : m_workers) {
        worker.join();
    }
}

SamplePool& SamplePool::global() {
    static SamplePool pool;
    return pool;
}

std::shared_ptr<const SampleEntry> SamplePool::request(const std::filesystem::path& path) {
    std::error_code ignored;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ignored);
    const std::string key = (canonical.empty() ? path : canonical).generic_string();

    const std::lock_guard lock(m_mutex);
    ++m_stats.requests;
    if (const auto found = m_byPath.find(key); found != m_byPath.end()) {
        if (std::shared_ptr<SampleEntry> live = found->second.lock()) {
            return live;
        }
    }
    auto entry = std::make_shared<SampleEntry>(*this, canonical.empty() ? path : canonical);
    entry->m_key = key;
    m_byPath[key] = entry;
    ++m_stats.liveEntries;
    m_queue.push_back(entry);
    m_wake.notify_one();
    return entry;
}

std::filesystem::path SamplePool::resolve(const project::Resources& resources,
                                          std::string_view relative) const {
    const std::filesystem::path written{std::string(relative)};
    std::vector<std::filesystem::path> candidates;
    if (written.is_absolute()) {
        candidates.push_back(written);
    } else {
        candidates.push_back(std::filesystem::path{resources.baseDirectory} / written);
    }
    {
        const std::lock_guard lock(m_mutex);
        for (const std::filesystem::path& directory : m_searchPaths) {
            candidates.push_back(directory / written.relative_path());
            candidates.push_back(directory / written.filename());
        }
    }
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return candidates.front();
}

void SamplePool::addSearchPath(std::filesystem::path directory) {
    const std::lock_guard lock(m_mutex);
    m_searchPaths.push_back(std::move(directory));
}

void SamplePool::waitAll() {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] { return m_queue.empty() && m_inFlight == 0; });
}

SamplePoolStats SamplePool::stats() const {
    const std::lock_guard lock(m_mutex);
    SamplePoolStats stats = m_stats;
    stats.buffersFreed = m_freed->load();
    return stats;
}

void SamplePool::work() {
    for (;;) {
        std::shared_ptr<SampleEntry> entry;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_stopping) {
                return;
            }
            entry = m_queue.front().lock();
            m_queue.pop_front();
            if (!entry) {
                // Released before it was decoded: nothing to do.
                if (m_queue.empty() && m_inFlight == 0) {
                    m_idle.notify_all();
                }
                continue;
            }
            ++m_inFlight;
        }
        decode(entry);
        {
            const std::lock_guard lock(m_mutex);
            --m_inFlight;
            if (m_queue.empty() && m_inFlight == 0) {
                m_idle.notify_all();
            }
        }
    }
}

void SamplePool::decode(const std::shared_ptr<SampleEntry>& entry) {
    std::uint64_t content = 0;
    if (!hashFile(entry->m_path, content)) {
        entry->m_error = "cannot read '" + entry->m_path.string() + "'";
        entry->m_handle.fail();
        return;
    }
    {
        // The same bytes under another name: share the buffer that is already decoded.
        const std::lock_guard lock(m_mutex);
        if (const auto found = m_byContent.find(content); found != m_byContent.end()) {
            if (std::shared_ptr<const SampleBuffer> shared = found->second.lock()) {
                entry->m_buffer = std::move(shared);
                entry->m_handle.publish(entry->m_buffer->view());
                return;
            }
        }
    }

    auto decoded = std::make_shared<SampleBuffer>();
    std::string error;
    if (!decodeFile(entry->m_path, *decoded, error)) {
        entry->m_error = std::move(error);
        entry->m_handle.fail();
        return;
    }
    // A buffer counts itself freed when its last owner lets go - the exactly-once of
    // decode_dedup. The counter outlives the pool, so a buffer freed late still has
    // somewhere to count.
    const std::shared_ptr<std::atomic<std::uint64_t>> freed = m_freed;
    std::shared_ptr<const SampleBuffer> buffer(
        decoded.get(), [owner = decoded, freed](const SampleBuffer*) mutable {
            owner.reset();
            freed->fetch_add(1);
        });
    {
        const std::lock_guard lock(m_mutex);
        ++m_stats.decodes;
        m_byContent[content] = buffer;
    }
    entry->m_buffer = std::move(buffer);
    entry->m_handle.publish(entry->m_buffer->view());
}

void SamplePool::entryDestroyed(SampleEntry& entry) noexcept {
    const std::lock_guard lock(m_mutex);
    --m_stats.liveEntries;
    if (const auto found = m_byPath.find(entry.m_key);
        found != m_byPath.end() && found->second.expired()) {
        m_byPath.erase(found);
    }
}

} // namespace adx::format
