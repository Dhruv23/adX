// adx-thread: main
#include "engine/plugin/PluginHost.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "engine/plugin/Bridge/Platform.h"

namespace adx::plugin {

namespace {

/// Kept far below the deque's growth: a crash loop must not eat memory.
constexpr std::size_t kMaxNotices = 256;

std::string environment(const char* name) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc
    return out;
}

} // namespace

PluginHost& PluginHost::instance() {
    static PluginHost host;
    return host;
}

PluginHost::PluginHost() {
    m_executable = environment("ADX_PLUGIN_HOST");
    if (m_executable.empty()) {
        m_executable =
            (std::filesystem::path(bridge::thisModuleDirectory()) / "adx_plugin_host.exe").string();
    }
}

PluginHost::~PluginHost() {
    try {
        shutdown();
    } catch (...) { // NOLINT(bugprone-empty-catch): process exit; nothing to report to
    }
}

void PluginHost::setExecutable(std::string path) {
    const std::scoped_lock lock(m_mutex);
    m_executable = std::move(path);
}

std::string PluginHost::executable() const {
    const std::scoped_lock lock(m_mutex);
    return m_executable;
}

bool PluginHost::available() const {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(executable()), ec);
}

bool PluginHost::loadCache(const std::string& path, std::string& error) {
    const std::scoped_lock lock(m_mutex);
    m_cacheLoaded = true;
    return m_cache.load(path.empty() ? defaultCachePath() : path, error);
}

bool PluginHost::saveCache(std::string& error) {
    const std::scoped_lock lock(m_mutex);
    if (m_cache.path().empty()) {
        m_cache.setPath(defaultCachePath());
    }
    return m_cache.save(error);
}

std::size_t PluginHost::scan(const std::vector<std::string>& directories, bool force,
                             const ScanProgress& progress) {
    if (!m_cacheLoaded) {
        std::string ignored;
        static_cast<void>(loadCache({}, ignored));
    }
    ScanOptions options;
    options.executable = executable();
    options.force = force;
    const auto candidates = findPlugins(directories.empty() ? defaultSearchPaths() : directories);
    std::size_t scanned = 0;
    {
        const std::scoped_lock lock(m_mutex);
        scanned = scanAll(candidates, m_cache, options, progress);
    }
    std::string ignored;
    static_cast<void>(saveCache(ignored));
    return scanned;
}

ScanResult PluginHost::rescan(const Candidate& candidate) {
    ScanOptions options;
    options.executable = executable();
    ScanResult result = scanFile(candidate, options);
    {
        const std::scoped_lock lock(m_mutex);
        m_cache.put(result);
    }
    std::string ignored;
    static_cast<void>(saveCache(ignored));
    return result;
}

const PluginDescriptor* PluginHost::describe(const PluginId& plugin) {
    {
        const std::scoped_lock lock(m_mutex);
        if (!m_cacheLoaded) {
            m_cacheLoaded = true;
            std::string ignored;
            static_cast<void>(m_cache.load(defaultCachePath(), ignored));
        }
        if (const PluginDescriptor* found = m_cache.findPlugin(plugin.format, plugin.uid)) {
            std::int64_t mtime = 0;
            std::uint64_t size = 0;
            const ScanResult* entry = m_cache.find(found->id.path);
            if (entry == nullptr || !bridge::fileStamp(found->id.path, mtime, size) ||
                (entry->mtime == mtime && entry->size == size)) {
                return found;
            }
        }
        // A failed file is not scanned again just because a project names it.
        if (const ScanResult* entry = m_cache.find(plugin.path); entry != nullptr && !entry->ok) {
            std::int64_t mtime = 0;
            std::uint64_t size = 0;
            if (bridge::fileStamp(plugin.path, mtime, size) && entry->mtime == mtime &&
                entry->size == size) {
                return nullptr;
            }
        }
    }
    if (plugin.path.empty()) {
        return nullptr;
    }
    static_cast<void>(rescan(Candidate{.format = plugin.format, .path = plugin.path}));
    const std::scoped_lock lock(m_mutex);
    return m_cache.findPlugin(plugin.format, plugin.uid);
}

std::shared_ptr<bridge::HostProcess> PluginHost::newChild(std::string& error) {
    // The caller holds m_mutex.
    auto child = std::make_shared<bridge::HostProcess>(
        m_executable, [this](const bridge::Notice& n) { notice(n); });
    if (!child->start(error)) {
        return nullptr;
    }
    m_children.push_back(child);
    return child;
}

std::shared_ptr<bridge::HostProcess> PluginHost::sharedChild(std::string& error) {
    if (auto existing = m_shared.lock(); existing && existing->alive()) {
        return existing;
    }
    auto child = newChild(error);
    m_shared = child;
    return child;
}

std::shared_ptr<bridge::RemoteInstance>
PluginHost::create(const PluginId& plugin, const StateBlob* state, std::uint32_t sampleRate,
                   std::uint32_t maxFrames, bool isolated, std::string& error) {
    std::shared_ptr<bridge::HostProcess> child;
    {
        const std::scoped_lock lock(m_mutex);
        std::erase_if(m_children, [](const auto& weak) { return weak.expired(); });
        child = isolated ? newChild(error) : sharedChild(error);
    }
    if (!child) {
        return nullptr;
    }
    auto instance = child->load(plugin, state, sampleRate, maxFrames, error);
    if (!instance) {
        return nullptr;
    }
    instance->setIsolated(isolated);
    const std::scoped_lock lock(m_mutex);
    std::erase_if(m_instances, [](const auto& weak) { return weak.expired(); });
    m_instances.push_back(instance);
    return instance;
}

std::shared_ptr<bridge::RemoteInstance> PluginHost::find(std::uint32_t id) const {
    const std::scoped_lock lock(m_mutex);
    for (const auto& weak : m_instances) {
        if (auto locked = weak.lock(); locked && locked->id() == id) {
            return locked;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<bridge::RemoteInstance>> PluginHost::instances() const {
    std::vector<std::shared_ptr<bridge::RemoteInstance>> out;
    const std::scoped_lock lock(m_mutex);
    for (const auto& weak : m_instances) {
        if (auto locked = weak.lock()) {
            out.push_back(std::move(locked));
        }
    }
    return out;
}

std::size_t PluginHost::restart(std::uint32_t instance, std::string& error) {
    std::vector<std::shared_ptr<bridge::HostProcess>> targets;
    {
        const std::scoped_lock lock(m_mutex);
        for (const auto& weak : m_children) {
            auto child = weak.lock();
            if (!child) {
                continue;
            }
            if (instance != 0) {
                const auto match = std::ranges::find_if(m_instances, [&](const auto& w) {
                    const auto locked = w.lock();
                    return locked && locked->id() == instance && locked->host() == child;
                });
                if (match != m_instances.end()) {
                    targets.push_back(child);
                }
            } else if (child->crashed() || !child->alive()) {
                targets.push_back(child);
            }
        }
    }
    std::size_t restarted = 0;
    for (const auto& child : targets) {
        std::vector<std::shared_ptr<bridge::RemoteInstance>> isolated;
        if (child->respawn(error, &isolated)) {
            ++restarted;
        }
        for (const auto& moved : isolated) {
            std::shared_ptr<bridge::HostProcess> own;
            {
                const std::scoped_lock lock(m_mutex);
                own = newChild(error);
            }
            if (own && own->adopt(moved, error)) {
                ++restarted;
                notice(bridge::Notice{.kind = bridge::Notice::Kind::Restarted,
                                      .instance = moved->id(),
                                      .plugin = moved->plugin().name,
                                      .message = "restarted in a process of its own"});
            }
        }
    }
    return restarted;
}

std::int64_t PluginHost::checkLatency(std::uint32_t id) {
    const auto instance = find(id);
    std::uint32_t measured = 0;
    if (!instance || !bridge::measureLatency(*instance, measured)) {
        return -1;
    }
    const std::uint32_t reported = instance->latency();
    if (measured != reported) {
        notice(bridge::Notice{.kind = bridge::Notice::Kind::LatencyMismatch,
                              .instance = id,
                              .plugin = instance->plugin().name,
                              .message = "reports " + std::to_string(reported) +
                                         " samples of latency but measures " +
                                         std::to_string(measured) +
                                         "; delay compensation will be off by the difference"});
    }
    return measured;
}

void PluginHost::notice(const bridge::Notice& notice) {
    const std::scoped_lock lock(m_noticeMutex);
    if (m_notices.size() >= kMaxNotices) {
        m_notices.pop_front();
    }
    m_notices.push_back(notice);
}

std::vector<bridge::Notice> PluginHost::takeNotices() {
    const std::scoped_lock lock(m_noticeMutex);
    std::vector<bridge::Notice> out(m_notices.begin(), m_notices.end());
    m_notices.clear();
    return out;
}

std::uint32_t PluginHost::deadlineMicros(std::uint32_t sampleRate, std::uint32_t frames,
                                         bool offline) noexcept {
    return bridge::deadlineMicros(sampleRate, frames, offline);
}

void PluginHost::shutdown() {
    std::vector<std::shared_ptr<bridge::HostProcess>> children;
    {
        const std::scoped_lock lock(m_mutex);
        for (const auto& weak : m_children) {
            if (auto child = weak.lock()) {
                children.push_back(std::move(child));
            }
        }
        m_children.clear();
        m_shared.reset();
    }
    // The last reference to each child ends here unless an instance still holds one,
    // and a child's destructor quits it.
}

} // namespace adx::plugin
