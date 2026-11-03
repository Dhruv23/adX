// adx-thread: main
//
// What a scan learns about a plugin, and what the cache keeps (phase_9.md §4.2).
//
// The parameter list is part of it, because a slot's parameters are laid out from it
// when the graph is built: a plugin parameter has a place in the parameter storage
// whether or not the project has given it a value yet, so it can be automated and
// MIDI-learned like a native one (§4.7).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/plugin/PluginId.h"

namespace adx::plugin {

namespace json {
struct Value;
}

struct ParamInfo {
    /// The plugin's own id for it (clap_id, Vst::ParamID).
    std::uint32_t id{0};
    /// What the .adx file calls it: the name, lower case, with anything that is not a
    /// letter, a digit or '_' made '_'. Unique within the plugin (paramKey).
    std::string key;
    std::string name;
    /// In the plugin's own units: CLAP's plain value, VST3's normalised 0..1.
    double minimum{0.0};
    double maximum{1.0};
    double defaultValue{0.0};
    /// Non-zero for a stepped parameter: how many steps.
    std::uint32_t steps{0};
    bool automatable{true};
    bool hidden{false};

    [[nodiscard]] friend bool operator==(const ParamInfo&, const ParamInfo&) noexcept = default;
};

struct PluginDescriptor {
    PluginId id;
    std::string vendor;
    bool instrument{false};
    std::uint32_t audioInputs{2};
    std::uint32_t audioOutputs{2};
    bool hasEditor{false};
    /// As reported at activation, at 48 kHz. The live instance reports its own.
    std::uint32_t latency{0};
    std::vector<ParamInfo> params;

    [[nodiscard]] const ParamInfo* findParam(std::string_view key) const noexcept;

    [[nodiscard]] friend bool operator==(const PluginDescriptor&,
                                         const PluginDescriptor&) noexcept = default;
};

/// One file's scan: every plugin in it, or why there are none.
struct ScanResult {
    std::string path;
    PluginFormat format{PluginFormat::Clap};
    std::int64_t mtime{0};
    std::uint64_t size{0};
    bool ok{false};
    std::string error;
    std::vector<PluginDescriptor> plugins;

    [[nodiscard]] friend bool operator==(const ScanResult&, const ScanResult&) noexcept = default;
};

/// The .adx key for a parameter named `name`, made unique against `taken` by a
/// numeric suffix. Deterministic, so a rescan yields the same keys.
[[nodiscard]] std::string paramKey(std::string_view name, std::uint32_t id,
                                   const std::vector<ParamInfo>& taken);

/// JSON, the scanner's wire format and the cache's file format.
[[nodiscard]] std::string toJson(const ScanResult& result);
[[nodiscard]] bool fromJson(std::string_view text, ScanResult& out, std::string& error);
[[nodiscard]] bool fromJson(const json::Value& root, ScanResult& out, std::string& error);

} // namespace adx::plugin
