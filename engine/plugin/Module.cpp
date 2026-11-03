// adx-thread: main
#include <string>

#include "engine/plugin/Instance.h"
#include "engine/plugin/clap/ClapModule.h"
#include "engine/plugin/vst3/Vst3Module.h"

namespace adx::plugin {

std::unique_ptr<Module> loadModule(PluginFormat format, const std::string& path,
                                   std::string& error) {
    switch (format) {
    case PluginFormat::Clap:
        return clap::loadClap(path, error);
    case PluginFormat::Vst3:
        return vst3::loadVst3(path, error);
    }
    error = "unknown plugin format";
    return nullptr;
}

void assignParamKeys(std::vector<ParamInfo>& params) {
    std::vector<ParamInfo> taken;
    taken.reserve(params.size());
    for (ParamInfo& param : params) {
        param.key = paramKey(param.name, param.id, taken);
        taken.push_back(param);
    }
}

} // namespace adx::plugin
