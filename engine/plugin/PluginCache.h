// adx-thread: main
//
// What every scanned plugin file held, on disk, so startup does not rescan
// (phase_9.md §4.2). Keyed by path and invalidated by the file's mtime and size: a
// plugin the user updated is scanned again, one they did not is not.
//
// A failed scan is kept too, with its reason. That is the blacklist: a file that
// crashed or hung the scanner is skipped next time unless it changed or the user asks
// for it to be rescanned - visible and editable, because a plugin that failed once may
// work after an update.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/plugin/PluginDescriptor.h"

namespace adx::plugin {

class PluginCache {
public:
    /// Reads `path`. A missing file is an empty cache, not an error; a malformed one is
    /// reported and treated as empty (it is rebuilt by the next scan).
    [[nodiscard]] bool load(const std::string& path, std::string& error);
    [[nodiscard]] bool save(std::string& error) const;
    [[nodiscard]] const std::string& path() const noexcept {
        return m_path;
    }
    void setPath(std::string path) {
        m_path = std::move(path);
    }

    [[nodiscard]] const ScanResult* find(std::string_view file) const noexcept;
    /// True when `file` was scanned as it is now (same mtime and size).
    [[nodiscard]] bool fresh(std::string_view file, std::int64_t mtime,
                             std::uint64_t size) const noexcept;
    void put(ScanResult result);
    bool remove(std::string_view file);

    [[nodiscard]] const std::vector<ScanResult>& entries() const noexcept {
        return m_entries;
    }
    /// The failed entries.
    [[nodiscard]] std::vector<const ScanResult*> blacklist() const;
    /// A plugin by identity, from any successful entry.
    [[nodiscard]] const PluginDescriptor* findPlugin(PluginFormat format,
                                                     std::string_view uid) const noexcept;
    [[nodiscard]] std::vector<const PluginDescriptor*> plugins() const;

    [[nodiscard]] std::string toJson() const;
    [[nodiscard]] bool fromJson(std::string_view text, std::string& error);

private:
    std::string m_path;
    std::vector<ScanResult> m_entries;
};

/// %LOCALAPPDATA%\adX\plugin-cache.json, or the ADX_PLUGIN_CACHE environment variable.
[[nodiscard]] std::string defaultCachePath();

} // namespace adx::plugin
