// adx-thread: main
#include "engine/instruments/voice/VoiceRenderCache.h"

#include <utility>

namespace adx::instruments {

VoiceRenderCache::VoiceRenderCache(std::size_t workers) {
    m_workers.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w) {
        m_workers.emplace_back([this](const std::stop_token& stop) { work(stop); });
    }
}

VoiceRenderCache::~VoiceRenderCache() {
    for (std::jthread& worker : m_workers) {
        worker.request_stop();
    }
    m_wake.notify_all();
    m_workers.clear(); // joins
}

VoiceRenderCache& VoiceRenderCache::global() {
    static VoiceRenderCache cache;
    return cache;
}

std::shared_ptr<const VoiceRenderCache::Entry>
VoiceRenderCache::request(const NoteRenderRequest& request) {
    const std::uint64_t key = renderKey(request);
    const std::scoped_lock lock(m_mutex);
    if (const auto found = m_entries.find(key); found != m_entries.end()) {
        return found->second;
    }
    auto entry = std::make_shared<Entry>();
    entry->key = key;
    m_entries.emplace(key, entry);
    m_queue.push_back(Job{.entry = entry, .request = request});
    m_wake.notify_one();
    return entry;
}

void VoiceRenderCache::waitAll() {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] { return m_queue.empty() && m_running == 0; });
}

std::size_t VoiceRenderCache::rendersDone() const {
    const std::scoped_lock lock(m_mutex);
    return m_done;
}

void VoiceRenderCache::work(const std::stop_token& stop) {
    while (true) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            if (!m_wake.wait(lock, stop, [this] { return !m_queue.empty(); })) {
                return; // stopping
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
            ++m_running;
        }
        NoteRender render = renderNote(job.request);
        Entry& entry = *job.entry;
        entry.samples = std::move(render.samples);
        entry.ok = render.ok;
        entry.clip.samples = entry.samples.data();
        entry.clip.frames = static_cast<std::uint32_t>(entry.samples.size());
        entry.clip.lead = render.lead;
        entry.clip.ready.store(true, std::memory_order_release);
        {
            const std::scoped_lock lock(m_mutex);
            --m_running;
            ++m_done;
            if (m_queue.empty() && m_running == 0) {
                m_idle.notify_all();
            }
        }
    }
}

} // namespace adx::instruments
