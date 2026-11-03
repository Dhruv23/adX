// adx-thread: main
#include "engine/plugin/Scanner.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#include "engine/plugin/Bridge/Platform.h"

namespace adx::plugin {

namespace {

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

std::filesystem::path fromUtf8(const std::string& text) {
    return {std::u8string(text.begin(), text.end())};
}

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

ScanResult failed(const Candidate& candidate, std::string reason) {
    ScanResult result;
    result.path = candidate.path;
    result.format = candidate.format;
    static_cast<void>(bridge::fileStamp(candidate.path, result.mtime, result.size));
    result.ok = false;
    result.error = std::move(reason);
    return result;
}

} // namespace

ScanResult scanFile(const Candidate& candidate, const ScanOptions& options) {
    bridge::ChildProcess child;
    std::string error;
    if (!child.spawn(options.executable, {"--scan", toString(candidate.format), candidate.path},
                     error)) {
        return failed(candidate, error);
    }
    std::string output;
    if (!child.readAll(output, options.deadlineMillis)) {
        child.kill();
        return failed(candidate, "timed out after " +
                                     std::to_string(options.deadlineMillis / 1000) +
                                     " s (the plugin hung, or opened a dialog)");
    }
    if (!child.waitExit(options.deadlineMillis)) {
        child.kill();
    }
    ScanResult result;
    std::string parseError;
    if (output.empty() || !fromJson(output, result, parseError)) {
        const std::uint32_t code = child.exitCode();
        std::string reason = output.empty() ? "the scanner crashed" : "malformed scan output";
        if (code != 0) {
            std::array<char, 16> hex{};
            std::snprintf(hex.data(), hex.size(), "0x%08X", code);
            reason += " (exit code " + std::string(hex.data()) + ")";
        }
        return failed(candidate, reason);
    }
    // The child stamped the file as it found it; keep the request's own path spelling.
    result.path = candidate.path;
    result.format = candidate.format;
    return result;
}

std::size_t scanAll(const std::vector<Candidate>& candidates, PluginCache& cache,
                    const ScanOptions& options, const ScanProgress& progress) {
    std::vector<const Candidate*> todo;
    for (const Candidate& candidate : candidates) {
        std::int64_t mtime = 0;
        std::uint64_t size = 0;
        if (!bridge::fileStamp(candidate.path, mtime, size)) {
            continue;
        }
        if (!options.force && cache.fresh(candidate.path, mtime, size)) {
            continue;
        }
        todo.push_back(&candidate);
    }
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::mutex resultMutex;
    const auto worker = [&] {
        while (true) {
            const std::size_t index = next.fetch_add(1);
            if (index >= todo.size()) {
                return;
            }
            ScanResult result = scanFile(*todo[index], options);
            const std::string path = result.path;
            {
                const std::scoped_lock lock(resultMutex);
                cache.put(std::move(result));
                const std::size_t finished = done.fetch_add(1) + 1;
                if (progress) {
                    progress(finished, todo.size(), path);
                }
            }
        }
    };
    const std::size_t threads =
        std::clamp<std::size_t>(options.parallel, 1, std::max<std::size_t>(1, todo.size()));
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        pool.emplace_back(worker);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    return todo.size();
}

std::vector<Candidate> findPlugins(const std::vector<std::string>& directories) {
    namespace fs = std::filesystem;
    std::vector<Candidate> out;
    for (const std::string& directory : directories) {
        std::error_code ec;
        const fs::path root = fromUtf8(directory);
        if (!fs::is_directory(root, ec)) {
            continue;
        }
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                            ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const fs::path& path = it->path();
            std::string extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            if (extension == ".clap" && it->is_regular_file(ec)) {
                out.push_back(Candidate{.format = PluginFormat::Clap, .path = toUtf8(path)});
            } else if (extension == ".vst3") {
                // A bundle is a directory; its insides are the loader's business.
                out.push_back(Candidate{.format = PluginFormat::Vst3, .path = toUtf8(path)});
                if (it->is_directory(ec)) {
                    it.disable_recursion_pending();
                }
            }
        }
    }
    std::ranges::sort(out, {}, &Candidate::path);
    const auto [first, last] = std::ranges::unique(out, {}, &Candidate::path);
    out.erase(first, last);
    return out;
}

std::vector<std::string> defaultSearchPaths() {
    std::vector<std::string> out;
    const std::string common = environment("COMMONPROGRAMFILES");
    const std::string local = environment("LOCALAPPDATA");
    if (!common.empty()) {
        out.push_back(toUtf8(fromUtf8(common) / "CLAP"));
        out.push_back(toUtf8(fromUtf8(common) / "VST3"));
    }
    if (!local.empty()) {
        out.push_back(toUtf8(fromUtf8(local) / "Programs" / "Common" / "CLAP"));
        out.push_back(toUtf8(fromUtf8(local) / "Programs" / "Common" / "VST3"));
    }
    const std::string extra = environment("ADX_PLUGIN_PATH");
    std::size_t start = 0;
    while (start < extra.size()) {
        const std::size_t end = extra.find(';', start);
        const std::string part =
            extra.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) {
            out.push_back(part);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

} // namespace adx::plugin
