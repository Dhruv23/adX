// adx-thread: main
//
// CLAP hosting (phase_9.md §3 step 8), in the sandbox child: a .clap file's entry and
// factory, and an Instance over one clap_plugin. CLAP is a C ABI with header-only
// definitions, so this is the whole of it.
#pragma once

#include <memory>
#include <string>

#include "engine/plugin/Instance.h"

namespace adx::plugin::clap {

[[nodiscard]] std::unique_ptr<Module> loadClap(const std::string& path, std::string& error);

} // namespace adx::plugin::clap
