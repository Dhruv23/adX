// adx-thread: main
//
// The format-agnostic face of plugin hosting (phase_9.md §2): what the graph and the
// bindings see. One per process. It owns the sandbox children, the scan cache, and the
// queue of notices (a crash, a hang, a muted NaN) the app shows the user.
//
// Instances share one child by default (§4.3); a plugin marked isolated gets a child
// of its own. A frozen channel loads nothing at all.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/plugin/Bridge/HostProcess.h"
#include "engine/plugin/PluginCache.h"
#include "engine/plugin/Scanner.h"

namespace adx::plugin {

class PluginHost {
public:
    static PluginHost& instance();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;
    PluginHost(PluginHost&&) = delete;
    PluginHost& operator=(PluginHost&&) = delete;
    ~PluginHost();

    /// adx_plugin_host.exe: ADX_PLUGIN_HOST, else beside this module.
    void setExecutable(std::string path);
    [[nodiscard]] std::string executable() const;
    [[nodiscard]] bool available() const;

    [[nodiscard]] PluginCache& cache() noexcept {
        return m_cache;
    }
    /// Loads the cache from `path` (defaultCachePath() when empty).
    bool loadCache(const std::string& path, std::string& error);
    bool saveCache(std::string& error);

    /// Scans `directories` (defaultSearchPaths() when empty) into the cache and saves it.
    std::size_t scan(const std::vector<std::string>& directories, bool force,
                     const ScanProgress& progress = {});
    /// Scans one file now, whatever the cache says. Saves the cache.
    ScanResult rescan(const Candidate& candidate);

    /// The descriptor for `plugin`: from the cache, else scanned now from its path.
    /// Null when it cannot be found or fails to scan.
    [[nodiscard]] const PluginDescriptor* describe(const PluginId& plugin);

    /// A running instance of `plugin` with `state` loaded (when non-empty).
    [[nodiscard]] std::shared_ptr<bridge::RemoteInstance>
    create(const PluginId& plugin, const StateBlob* state, std::uint32_t sampleRate,
           std::uint32_t maxFrames, bool isolated, std::string& error);

    /// Every live instance, by id, for the UI's per-plugin readouts.
    [[nodiscard]] std::shared_ptr<bridge::RemoteInstance> find(std::uint32_t id) const;
    [[nodiscard]] std::vector<std::shared_ptr<bridge::RemoteInstance>> instances() const;

    /// Restarts every child whose plugins crashed (or the one holding `instance`, when
    /// non-zero), moving instances marked isolated to children of their own. Returns
    /// how many children came back.
    std::size_t restart(std::uint32_t instance, std::string& error);

    /// Measures instance `id`'s latency with an impulse (bridge::measureLatency) and
    /// posts a LatencyMismatch notice when it differs from the report. Only while the
    /// instance is not being rendered. Returns the measurement, or -1 when there was none.
    std::int64_t checkLatency(std::uint32_t id);

    [[nodiscard]] std::vector<bridge::Notice> takeNotices();
    void notice(const bridge::Notice& notice);

    /// How long the audio thread waits for one block, in microseconds: half the
    /// block's duration in realtime (other nodes need the rest), ten seconds offline.
    [[nodiscard]] static std::uint32_t deadlineMicros(std::uint32_t sampleRate,
                                                      std::uint32_t frames, bool offline) noexcept;

    /// Quits every child. Instances still held are left running nothing.
    void shutdown();

private:
    PluginHost();
    [[nodiscard]] std::shared_ptr<bridge::HostProcess> sharedChild(std::string& error);
    [[nodiscard]] std::shared_ptr<bridge::HostProcess> newChild(std::string& error);

    mutable std::mutex m_mutex;
    std::string m_executable;
    PluginCache m_cache;
    bool m_cacheLoaded{false};
    std::weak_ptr<bridge::HostProcess> m_shared;
    std::vector<std::weak_ptr<bridge::HostProcess>> m_children;
    std::vector<std::weak_ptr<bridge::RemoteInstance>> m_instances;
    std::mutex m_noticeMutex;
    std::deque<bridge::Notice> m_notices;
};

} // namespace adx::plugin
