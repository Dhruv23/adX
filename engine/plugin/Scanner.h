// adx-thread: main
//
// Plugin scanning, out of process, always (phase_9.md §4.2). Each file is loaded by a
// fresh `adx_plugin_host --scan` under a deadline; a crash, a hang or output that is
// not a scan result records the file as failed with the reason, and the scan goes on.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine/plugin/PluginCache.h"
#include "engine/plugin/PluginDescriptor.h"

namespace adx::plugin {

struct ScanOptions {
    /// adx_plugin_host.exe.
    std::string executable;
    std::uint32_t deadlineMillis{30'000};
    std::uint32_t parallel{4};
    /// Rescan files the cache already has, failed ones included.
    bool force{false};
};

struct Candidate {
    PluginFormat format{PluginFormat::Clap};
    std::string path;
};

/// Scans one file. Never throws, never crashes this process: whatever the plugin does,
/// the answer is a ScanResult.
[[nodiscard]] ScanResult scanFile(const Candidate& candidate, const ScanOptions& options);

/// (done, total, path just finished).
using ScanProgress = std::function<void(std::size_t, std::size_t, const std::string&)>;

/// Scans every candidate the cache does not have fresh (all of them with `force`),
/// `parallel` at a time, and puts each result in `cache`. Returns how many were scanned.
std::size_t scanAll(const std::vector<Candidate>& candidates, PluginCache& cache,
                    const ScanOptions& options, const ScanProgress& progress = {});

/// .clap files and .vst3 files or bundles under `directories`, recursively.
[[nodiscard]] std::vector<Candidate> findPlugins(const std::vector<std::string>& directories);

/// Where Windows plugins live: the CLAP and VST3 common folders, system-wide and per
/// user, and anything in ADX_PLUGIN_PATH (separated by ';').
[[nodiscard]] std::vector<std::string> defaultSearchPaths();

} // namespace adx::plugin
