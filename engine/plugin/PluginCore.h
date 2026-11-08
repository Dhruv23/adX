// adx-thread: realtime
//
// What a plugin effect node and a plugin instrument node share (phase_9.md §4.5): the
// link to the sandboxed instance, the parameter slice turned into change events, the
// late-block rule, and the NaN guard. The node decides what a block that did not come
// back sounds like - dry for an effect, silence for an instrument.
//
// The main-thread half - which plugin, its descriptor, the RemoteInstance that keeps
// the child's objects alive - is a PluginBinding, opaque here and defined beside the
// factory in PluginBinding.cpp.
#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>

#include "engine/graph/Node.h"
#include "engine/plugin/Bridge/AudioLink.h"
#include "engine/rt/OwnedArray.h"
#include "engine/rt/SpscRing.h"
#include "engine/transport/TimeSource.h"

namespace adx::project {
struct PluginRef;
}

namespace adx::plugin {

struct PluginBinding;

/// A parameter the plugin changed itself, on its way to the main thread.
struct ParamEcho {
    std::uint32_t index;
    float value;
    /// Inside an edit gesture: the user's hand, not the plugin reporting itself.
    bool gesture;
    /// In the instance's first half second: a freshly loaded plugin telling adX what
    /// it loaded (P10.5-2), unless it is a gesture.
    bool settling;
};

enum class CoreResult : std::uint8_t {
    /// The plugin's output is in `outputs`.
    Played,
    /// Silence is in `outputs`: late, or muted for NaN.
    Silent,
    /// Nothing was written: not running (loading, crashed, bypassed for lateness).
    Absent,
};

class PluginCore {
public:
    PluginCore() = default;
    ~PluginCore();
    PluginCore(const PluginCore&) = delete;
    PluginCore& operator=(const PluginCore&) = delete;
    PluginCore(PluginCore&&) = delete;
    PluginCore& operator=(PluginCore&&) = delete;

    // --- main thread ---

    /// Which plugin, as the slot or instrument names it. `format` is its type.
    void configure(std::string_view format, const project::PluginRef& ref, bool instrument);
    /// Loads the instance in a sandbox child. A failure leaves the core absent (the
    /// node plays dry or silent) and is reported as a notice.
    void prepare(const graph::PrepareInfo& info);
    [[nodiscard]] bool matches(std::string_view format,
                               const project::PluginRef& ref) const noexcept;
    void copyConfigTo(PluginCore& fresh) const;
    [[nodiscard]] std::uint32_t paramCount() const noexcept;
    [[nodiscard]] std::string_view paramKey(std::uint32_t index) const noexcept;
    [[nodiscard]] std::uint32_t latency() const noexcept;
    [[nodiscard]] std::string_view formatName() const noexcept;
    /// The live instance's id in PluginHost, or 0 when none is loaded.
    [[nodiscard]] std::uint32_t instanceId() const noexcept;
    /// One echo, or false when none is waiting.
    [[nodiscard]] bool takeEcho(ParamEcho& out) const noexcept {
        return m_echoes.tryPop(out);
    }
    [[nodiscard]] PluginBinding* binding() const noexcept {
        return m_binding;
    }

    // --- audio thread ---

    /// One block. `params` is the node's declared slice (NaN for "leave it"),
    /// `automation` per-frame values where they move. `events` must be sorted.
    [[nodiscard]] CoreResult process(std::span<const float* const> inputs,
                                     std::span<float* const> outputs, std::uint32_t frames,
                                     std::span<const float> params,
                                     std::span<const float* const> automation,
                                     std::span<const bridge::WireEvent> events,
                                     const transport::TimeSource& time) noexcept;
    /// The plugin's current latency, read from the shared region.
    [[nodiscard]] std::uint32_t liveLatency() const noexcept;

private:
    void collectParams(std::span<const float> params, std::span<const float* const> automation,
                       std::uint32_t frames) noexcept;

    PluginBinding* m_binding{nullptr};
    bridge::AudioLink m_link{};
    bool m_offline{false};
    std::uint32_t m_sampleRate{48000};
    std::uint32_t m_generation{0};
    /// Frames this instance (this generation of it) has played.
    std::uint64_t m_played{0};
    bridge::LateCounter m_late;
    /// Per declared parameter: the plugin id, and the value last sent (NaN: never).
    rt::OwnedArray<std::uint32_t> m_ids;
    rt::OwnedArray<float> m_sent;
    rt::OwnedArray<bridge::WireParam> m_changes;
    std::uint32_t m_changeCount{0};
    rt::OwnedArray<bridge::WireParam> m_outParams;
    /// Mutable: the main thread consumes it through a const node (NodeStore hands out
    /// const nodes to the UI); it is the ring's only consumer.
    mutable rt::SpscRing<ParamEcho, 256> m_echoes;
};

/// Plugin id -> declared index, for echoes. Main thread.
[[nodiscard]] std::uint32_t indexOfParamId(const PluginCore& core, std::uint32_t id) noexcept;

} // namespace adx::plugin
