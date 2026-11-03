// Which third-party plugin a slot or a channel hosts, and its saved state
// (phase_9.md §4.7).
//
// Only what the project needs to name the plugin and bring it back: the format is the
// slot's or instrument's type ("clap", "vst3"), the uid is its identity, the path is
// where it was found and the name is for a reader of the file. The plugin's named
// parameters are ordinary SlotParam / ParamValue entries beside this; the state is the
// opaque remainder, kept as base64 text exactly as the file holds it.
#pragma once

#include <string>
#include <string_view>

namespace adx::project {

struct PluginRef {
    std::string uid;
    std::string path;
    std::string name;
    /// base64, without the `base64:` prefix. Empty when the plugin has never been saved.
    std::string state;
    /// Hosted in a sandbox child of its own instead of the shared one.
    bool isolated{false};

    [[nodiscard]] bool empty() const noexcept {
        return uid.empty();
    }

    [[nodiscard]] friend bool operator==(const PluginRef&, const PluginRef&) noexcept = default;
};

/// True for the slot and instrument types that name a hosted plugin.
[[nodiscard]] inline bool isPluginType(std::string_view type) noexcept {
    return type == "clap" || type == "vst3";
}

} // namespace adx::project
