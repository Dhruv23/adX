// adx-thread: main
#include "engine/plugin/PluginCache.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "engine/plugin/Json.h"

namespace adx::plugin {

namespace {

constexpr int kCacheVersion = 1;

std::string environment(const char* name) {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc
    return out;
}

} // namespace

std::string defaultCachePath() {
    if (std::string forced = environment("ADX_PLUGIN_CACHE"); !forced.empty()) {
        return forced;
    }
    std::string base = environment("LOCALAPPDATA");
    if (base.empty()) {
        base = ".";
    }
    return (std::filesystem::path(base) / "adX" / "plugin-cache.json").string();
}

const ScanResult* PluginCache::find(std::string_view file) const noexcept {
    const auto match = std::ranges::find(m_entries, file, &ScanResult::path);
    return match == m_entries.end() ? nullptr : &*match;
}

bool PluginCache::fresh(std::string_view file, std::int64_t mtime,
                        std::uint64_t size) const noexcept {
    const ScanResult* entry = find(file);
    return entry != nullptr && entry->mtime == mtime && entry->size == size;
}

void PluginCache::put(ScanResult result) {
    const auto match = std::ranges::find(m_entries, result.path, &ScanResult::path);
    if (match != m_entries.end()) {
        *match = std::move(result);
    } else {
        m_entries.push_back(std::move(result));
    }
    std::ranges::sort(m_entries, {}, &ScanResult::path);
}

bool PluginCache::remove(std::string_view file) {
    return std::erase_if(m_entries, [file](const ScanResult& r) { return r.path == file; }) > 0;
}

std::vector<const ScanResult*> PluginCache::blacklist() const {
    std::vector<const ScanResult*> out;
    for (const ScanResult& entry : m_entries) {
        if (!entry.ok) {
            out.push_back(&entry);
        }
    }
    return out;
}

const PluginDescriptor* PluginCache::findPlugin(PluginFormat format,
                                                std::string_view uid) const noexcept {
    for (const ScanResult& entry : m_entries) {
        for (const PluginDescriptor& plugin : entry.plugins) {
            if (plugin.id.format == format && plugin.id.uid == uid) {
                return &plugin;
            }
        }
    }
    return nullptr;
}

std::vector<const PluginDescriptor*> PluginCache::plugins() const {
    std::vector<const PluginDescriptor*> out;
    for (const ScanResult& entry : m_entries) {
        if (entry.ok) {
            for (const PluginDescriptor& plugin : entry.plugins) {
                out.push_back(&plugin);
            }
        }
    }
    return out;
}

std::string PluginCache::toJson() const {
    std::string out = "{\"version\":" + std::to_string(kCacheVersion) + ",\"entries\":[\n";
    for (std::size_t i = 0; i < m_entries.size(); ++i) {
        out += plugin::toJson(m_entries[i]);
        out += i + 1 < m_entries.size() ? ",\n" : "\n";
    }
    out += "]}\n";
    return out;
}

bool PluginCache::fromJson(std::string_view text, std::string& error) {
    json::Value root;
    if (!json::parse(text, root, error)) {
        return false;
    }
    if (root.num("version") != kCacheVersion) {
        error = "a plugin cache from another version; it will be rebuilt";
        return false;
    }
    const json::Value* entries = root.get("entries");
    if (entries == nullptr || entries->kind != json::Value::Kind::Array) {
        error = "the plugin cache has no entries";
        return false;
    }
    std::vector<ScanResult> loaded;
    for (const json::Value& item : entries->items) {
        ScanResult result;
        if (!plugin::fromJson(item, result, error)) {
            return false;
        }
        loaded.push_back(std::move(result));
    }
    m_entries = std::move(loaded);
    std::ranges::sort(m_entries, {}, &ScanResult::path);
    return true;
}

bool PluginCache::load(const std::string& path, std::string& error) {
    m_path = path;
    m_entries.clear();
    std::ifstream const file(std::filesystem::path(path), std::ios::binary);
    if (!file) {
        return true; // no cache yet
    }
    std::ostringstream text;
    text << file.rdbuf();
    if (!fromJson(text.str(), error)) {
        m_entries.clear();
        return false;
    }
    return true;
}

bool PluginCache::save(std::string& error) const {
    if (m_path.empty()) {
        error = "the plugin cache has no path";
        return false;
    }
    std::error_code ec;
    const std::filesystem::path target(m_path);
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    // Written beside and renamed over, so a crash mid-save leaves the old cache.
    const std::filesystem::path temporary = target.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "could not write " + temporary.string();
            return false;
        }
        file << toJson();
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        error = "could not replace " + m_path + ": " + ec.message();
        return false;
    }
    return true;
}

} // namespace adx::plugin
