// adx-thread: main
//
// A loaded plugin, format-agnostic - what the sandbox child drives (§4.3). The host
// never sees one of these: it sees the child, through the protocol. This is the seam
// that keeps CLAP and VST3 to one file each.
//
// Threads, as both formats define them: process() on the child's audio thread, every
// other call on its main thread.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/plugin/Bridge/SharedAudio.h"
#include "engine/plugin/PluginDescriptor.h"
#include "engine/plugin/StateBlob.h"

namespace adx::plugin {

struct ProcessIo {
    std::span<const float* const> inputs;
    std::span<float* const> outputs;
    std::uint32_t frames{0};
    std::span<const bridge::WireEvent> events;
    std::span<const bridge::WireParam> params;
    bridge::WireTransport transport{};
    /// Parameter changes the plugin made itself (its own editor): written here, `outCount`
    /// of them, at most outParams.size().
    std::span<bridge::WireParam> outParams;
    std::uint32_t outCount{0};
    /// Set by the instance: the plugin's latency changed or it asked to be restarted.
    std::uint32_t status{0};
};

class Instance {
public:
    Instance() = default;
    virtual ~Instance() = default;
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;
    Instance(Instance&&) = delete;
    Instance& operator=(Instance&&) = delete;

    /// Prepares to process at `sampleRate` in blocks of at most `maxFrames`.
    [[nodiscard]] virtual bool activate(double sampleRate, std::uint32_t maxFrames,
                                        std::string& error) = 0;
    virtual void deactivate() = 0;
    /// Deactivates and activates again at the same rate and block size: what a plugin
    /// that asked to be restarted, or changed its latency, needs. Main thread.
    [[nodiscard]] virtual bool restart(std::string& error) = 0;
    /// Child audio thread.
    virtual void process(ProcessIo& io) noexcept = 0;

    /// The descriptor of what is loaded: name, buses, parameters (with keys), latency.
    [[nodiscard]] virtual PluginDescriptor describe() = 0;
    [[nodiscard]] virtual bool paramValue(std::uint32_t id, double& out) = 0;
    [[nodiscard]] virtual bool paramText(std::uint32_t id, double value, std::string& out) = 0;
    [[nodiscard]] virtual bool saveState(StateBlob& out) = 0;
    [[nodiscard]] virtual bool loadState(std::span<const std::uint8_t> bytes) = 0;
    [[nodiscard]] virtual std::uint32_t latency() = 0;

    /// Main thread, every few milliseconds: whatever the plugin asked to have done on
    /// the main thread (CLAP's request_callback, a VST3 restartComponent).
    virtual void idle() {}

    /// Creates the plugin's editor in a window of the child's own and returns it, for
    /// the host to embed (§4.6). False when there is none.
    [[nodiscard]] virtual bool openEditor(std::uint64_t& window, std::uint32_t& width,
                                          std::uint32_t& height) {
        static_cast<void>(window);
        static_cast<void>(width);
        static_cast<void>(height);
        return false;
    }
    virtual void closeEditor() {}
};

/// One plugin file: describes what is in it, creates instances of it.
class Module {
public:
    Module() = default;
    virtual ~Module() = default;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    Module(Module&&) = delete;
    Module& operator=(Module&&) = delete;

    /// Every plugin in the file, each instantiated and activated once to learn its
    /// parameters and latency.
    [[nodiscard]] virtual std::vector<PluginDescriptor> scan(std::string& error) = 0;
    [[nodiscard]] virtual std::unique_ptr<Instance> create(const std::string& uid,
                                                           std::string& error) = 0;
};

/// Loads `path` as `format`. Null with `error` set when it is not a plugin of that kind.
[[nodiscard]] std::unique_ptr<Module> loadModule(PluginFormat format, const std::string& path,
                                                 std::string& error);

/// Fills in each parameter's key from its name, in order (paramKey).
void assignParamKeys(std::vector<ParamInfo>& params);

} // namespace adx::plugin
