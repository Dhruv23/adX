// adx-thread: main
//
// The main-thread half of a PluginCore: which plugin, what the cache says about it,
// and the RemoteInstance whose shared objects the audio thread's link points into.
#pragma once

#include <memory>
#include <string>

#include "engine/plugin/Bridge/HostProcess.h"
#include "engine/plugin/PluginDescriptor.h"
#include "engine/project/PluginRef.h"

namespace adx::plugin {

struct PluginBinding {
    PluginFormat format{PluginFormat::Clap};
    project::PluginRef ref;
    bool instrument{false};
    /// From the cache (or a scan at load): the declared parameter layout.
    PluginDescriptor descriptor;
    bool described{false};
    std::shared_ptr<bridge::RemoteInstance> instance;
    std::string error;

    [[nodiscard]] PluginId id() const {
        return PluginId{
            .format = format, .uid = ref.uid, .path = ref.path, .name = ref.name, .version = {}};
    }
};

} // namespace adx::plugin
