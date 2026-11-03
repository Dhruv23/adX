// adx-thread: main
//
// Which plugin, stably: its format, the id its own factory gives it, and the file it
// came from (phase_9.md §2). The uid is the identity; the path is where it was last
// found, so a plugin that moved is still the same plugin when a rescan finds it again.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace adx::plugin {

enum class PluginFormat : std::uint8_t { Clap, Vst3 };

[[nodiscard]] const char* toString(PluginFormat format) noexcept;
/// "clap" or "vst3", as a slot's type names it.
[[nodiscard]] bool formatFromString(std::string_view text, PluginFormat& out) noexcept;

struct PluginId {
    PluginFormat format{PluginFormat::Clap};
    /// CLAP: the plugin's reverse-DNS id. VST3: the processor class id, 32 hex digits.
    std::string uid;
    /// The .clap or .vst3 file, absolute. A .vst3 bundle directory is resolved to the
    /// module inside it by the loader, not here.
    std::string path;
    std::string name;
    std::string version;

    [[nodiscard]] friend bool operator==(const PluginId&, const PluginId&) noexcept = default;
};

} // namespace adx::plugin
