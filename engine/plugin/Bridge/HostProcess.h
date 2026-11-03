// adx-thread: main
//
// adX's side of one sandbox child (phase_9.md §4.3, §4.4): spawn, handshake, the
// request channel, and the watchdog that turns a crash, a hang or a run of late blocks
// into a notice and - for a hang - a restart.
//
// A RemoteInstance is one plugin instance in some child. It owns its shared region and
// its two events, so a child that dies and is replaced reattaches to the same memory:
// the audio thread's pointers (AudioLink) never change for the life of the instance.
// Instances share a child by default; one marked isolated moves to a child of its own
// at the next restart (§4.3, "configurable to one-child-per-plugin").
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engine/plugin/Bridge/AudioLink.h"
#include "engine/plugin/Bridge/Platform.h"
#include "engine/plugin/Bridge/Protocol.h"
#include "engine/plugin/PluginDescriptor.h"
#include "engine/plugin/StateBlob.h"

namespace adx::plugin::bridge {

class HostProcess;

struct Notice {
    enum class Kind : std::uint8_t {
        /// The child died; its instances are bypassed until restarted.
        Crashed,
        /// A block was outstanding for kHangMicros; the child was killed and restarted.
        Hung,
        /// A child came back and its instances were reloaded with their last state.
        Restarted,
        /// kLateLimit late blocks within the window; bypassed until re-enabled.
        LateBypassed,
        /// The plugin produced NaN or infinity; muted until re-enabled.
        NanMuted,
        /// The plugin's latency changed; the graph needs a new delay plan.
        LatencyChanged,
        /// The plugin rejected its saved state and runs with its defaults.
        StateRejected,
        /// An impulse took a different number of samples through the plugin than the
        /// latency it reports (measureLatency): PDC trusts the report, so it is wrong.
        LatencyMismatch,
    };
    Kind kind{Kind::Crashed};
    std::uint32_t instance{0};
    std::string plugin;
    std::string message;
};

using NoticeSink = std::function<void(const Notice&)>;

class RemoteInstance {
public:
    RemoteInstance(std::uint32_t id, PluginId plugin, std::uint32_t sampleRate,
                   std::uint32_t maxFrames);
    ~RemoteInstance();
    RemoteInstance(const RemoteInstance&) = delete;
    RemoteInstance& operator=(const RemoteInstance&) = delete;
    RemoteInstance(RemoteInstance&&) = delete;
    RemoteInstance& operator=(RemoteInstance&&) = delete;

    [[nodiscard]] std::uint32_t id() const noexcept {
        return m_id;
    }
    [[nodiscard]] const PluginId& plugin() const noexcept {
        return m_plugin;
    }
    /// What the live plugin reported at its last load.
    [[nodiscard]] const PluginDescriptor& descriptor() const noexcept {
        return m_descriptor;
    }
    [[nodiscard]] AudioLink link() noexcept;
    [[nodiscard]] LinkHealth& health() noexcept {
        return m_health;
    }
    [[nodiscard]] std::uint32_t latency() const noexcept;
    [[nodiscard]] std::uint32_t sampleRate() const noexcept {
        return m_sampleRate;
    }

    /// Asks the plugin for its state and remembers it as the one a restart restores.
    [[nodiscard]] bool saveState(StateBlob& out);
    /// Gives the plugin `bytes`; false when it rejected them (it keeps its defaults).
    [[nodiscard]] bool loadState(std::span<const std::uint8_t> bytes);
    [[nodiscard]] bool paramValues(std::vector<std::pair<std::uint32_t, double>>& out) const;
    [[nodiscard]] bool paramText(std::uint32_t id, double value, std::string& out) const;
    [[nodiscard]] bool openEditor(std::uint64_t& window, std::uint32_t& width,
                                  std::uint32_t& height) const;
    void closeEditor() const;

    /// A late-bypassed or muted instance plays again.
    void reenable() noexcept;

    /// Wants a child of its own from the next restart on.
    void setIsolated(bool isolated) noexcept {
        m_isolated = isolated;
    }
    [[nodiscard]] bool isolated() const noexcept {
        return m_isolated;
    }
    [[nodiscard]] std::shared_ptr<HostProcess> host() const;

private:
    friend class HostProcess;
    [[nodiscard]] bool createObjects();

    std::uint32_t m_id;
    PluginId m_plugin;
    PluginDescriptor m_descriptor;
    std::uint32_t m_sampleRate;
    std::uint32_t m_maxFrames;
    SharedRegion m_region;
    Event m_go;
    Event m_done;
    std::string m_regionName;
    std::string m_goName;
    std::string m_doneName;
    LinkHealth m_health;
    /// What a restart restores: the last state loaded or saved.
    StateBlob m_lastState;
    bool m_hasState{false};
    bool m_isolated{false};
    mutable std::mutex m_hostMutex;
    std::shared_ptr<HostProcess> m_host;
};

class HostProcess : public std::enable_shared_from_this<HostProcess> {
public:
    HostProcess(std::string executable, NoticeSink sink);
    ~HostProcess();
    HostProcess(const HostProcess&) = delete;
    HostProcess& operator=(const HostProcess&) = delete;
    HostProcess(HostProcess&&) = delete;
    HostProcess& operator=(HostProcess&&) = delete;

    /// Spawns the child and says hello. Starts the watchdog.
    [[nodiscard]] bool start(std::string& error);
    [[nodiscard]] bool alive() const noexcept;

    /// A new instance of `plugin` in this child, with `state` loaded when given.
    [[nodiscard]] std::shared_ptr<RemoteInstance> load(const PluginId& plugin,
                                                       const StateBlob* state,
                                                       std::uint32_t sampleRate,
                                                       std::uint32_t maxFrames, std::string& error);

    /// Kills the child (if it is not dead already), starts a new one and reloads every
    /// instance that still lives here with its last state. Instances marked isolated are
    /// left for the caller to move (they are returned).
    [[nodiscard]] bool respawn(std::string& error,
                               std::vector<std::shared_ptr<RemoteInstance>>* isolated = nullptr);

    /// Adopts `instance` (from another child) and loads it here.
    [[nodiscard]] bool adopt(const std::shared_ptr<RemoteInstance>& instance, std::string& error);

    /// One request and its reply. False on timeout or a dead child.
    [[nodiscard]] bool call(Op op, const ByteWriter& payload, Status& status,
                            std::vector<std::uint8_t>& reply, std::uint32_t timeoutMillis);

    /// How many instances live here.
    [[nodiscard]] std::size_t instanceCount() const;
    [[nodiscard]] bool crashed() const noexcept {
        return m_crashed.load();
    }
    /// For tests: the child's process handle, to watch it die.
    [[nodiscard]] std::uint32_t exitCode() const noexcept {
        return m_child.exitCode();
    }
    /// For tests: kills the child as a crash would.
    void killChild() noexcept {
        m_child.kill();
    }

private:
    friend class RemoteInstance;
    [[nodiscard]] bool spawnChild(std::string& error);
    [[nodiscard]] bool loadInto(RemoteInstance& instance, const StateBlob* state,
                                std::string& error);
    void unload(std::uint32_t id);
    void watch();
    void notify(Notice::Kind kind, const RemoteInstance& instance, std::string message);
    [[nodiscard]] std::vector<std::shared_ptr<RemoteInstance>> live() const;

    std::string m_executable;
    NoticeSink m_sink;
    ChildProcess m_child;
    std::recursive_mutex m_callMutex;
    std::uint32_t m_nextRequest{1};
    mutable std::mutex m_listMutex;
    std::vector<std::weak_ptr<RemoteInstance>> m_instances;
    std::atomic<bool> m_crashed{false};
    std::atomic<bool> m_respawning{false};
    /// The child was killed for hanging, so its exit is a restart, not a crash.
    std::atomic<bool> m_hungKill{false};
    std::atomic<bool> m_stop{false};
    std::thread m_watchdog;
};

/// Sends an impulse through `instance` and reports how many samples it took to come
/// out (the first sample at half the peak or more). False when nothing came back - an
/// instrument, a gate. Only while nothing else sends it blocks: the probe and the audio
/// thread must not both drive one link (AudioLink.h).
[[nodiscard]] bool measureLatency(RemoteInstance& instance, std::uint32_t& measured);

/// Instance ids are process-wide, so a notice names one instance whatever child it is in.
[[nodiscard]] std::uint32_t nextInstanceId() noexcept;

} // namespace adx::plugin::bridge
