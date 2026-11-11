// adx-thread: main
#include "engine/format/audio/LibraryPaths.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

namespace adx::format {
namespace {

std::string environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc
    return out;
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe)
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

} // namespace

std::string libraryOf(std::string_view path) {
    if (!isLibraryPath(path)) {
        return {};
    }
    path.remove_prefix(kLibraryScheme.size());
    return std::string(path.substr(0, path.find('/')));
}

std::filesystem::path libraryStore() {
    if (const std::string store = environment("ADX_LIBRARY_STORE"); !store.empty()) {
        return store;
    }
    if (const std::string local = environment("LOCALAPPDATA"); !local.empty()) {
        return std::filesystem::path(local) / "adX" / "libraries";
    }
    const std::string home = environment("HOME");
    return std::filesystem::path(home.empty() ? "." : home) / ".local" / "share" / "adX" /
           "libraries";
}

std::filesystem::path libraryFolder(const std::filesystem::path& store, std::string_view id) {
    std::error_code error;
    if (id.empty() || !std::filesystem::is_directory(store, error)) {
        return {};
    }
    if (std::filesystem::is_directory(store / id, error)) {
        return store / id;
    }
    const std::string prefix = std::string(id) + "@";
    std::vector<std::filesystem::path> found;
    for (const auto& entry : std::filesystem::directory_iterator(store, error)) {
        if (entry.is_directory(error) && entry.path().filename().string().starts_with(prefix)) {
            found.push_back(entry.path());
        }
    }
    if (found.empty()) {
        return {};
    }
    std::ranges::sort(found);
    return found.front();
}

std::filesystem::path resolveLibraryPath(const std::filesystem::path& store,
                                         std::string_view path) {
    const std::string id = libraryOf(path);
    const std::filesystem::path folder = libraryFolder(store, id);
    if (folder.empty()) {
        return {};
    }
    path.remove_prefix(kLibraryScheme.size() + id.size());
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    return folder / std::filesystem::path(path);
}

namespace {

struct FolderCache {
    std::mutex mutex;
    std::map<std::string, std::filesystem::path, std::less<>> folders;
};

FolderCache& folderCache() {
    static FolderCache cache;
    return cache;
}

} // namespace

std::filesystem::path resolveLibraryPath(std::string_view path) {
    const std::string id = libraryOf(path);
    if (id.empty()) {
        return {};
    }
    std::filesystem::path folder;
    {
        FolderCache& cache = folderCache();
        const std::lock_guard lock(cache.mutex);
        auto found = cache.folders.find(id);
        if (found == cache.folders.end()) {
            found = cache.folders.emplace(id, libraryFolder(libraryStore(), id)).first;
        }
        folder = found->second;
    }
    if (folder.empty()) {
        return {};
    }
    path.remove_prefix(kLibraryScheme.size() + id.size());
    while (!path.empty() && path.front() == '/') {
        path.remove_prefix(1);
    }
    return folder / std::filesystem::path(path);
}

void forgetLibraryFolders() {
    FolderCache& cache = folderCache();
    const std::lock_guard lock(cache.mutex);
    cache.folders.clear();
}

} // namespace adx::format
