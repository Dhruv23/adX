// adx-thread: main
//
// VST3 hosting (phase_9.md §3 step 7), in the sandbox child: a .vst3 module (a file or
// a bundle directory) through the SDK's hosting helpers, and an Instance over one
// audio-module class - its component, its controller, and the connection between them.
#pragma once

#include <memory>
#include <string>

#include "engine/plugin/Instance.h"

namespace adx::plugin::vst3 {

[[nodiscard]] std::unique_ptr<Module> loadVst3(const std::string& path, std::string& error);

} // namespace adx::plugin::vst3
