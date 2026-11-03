// adx-thread: main
#include "engine/render/Bundle.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <map>
#include <set>

#include "engine/format/adx/Document.h"
#include "engine/format/adx/Lexer.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/render/FileIo.h"

namespace adx::render {

namespace fs = std::filesystem;

namespace {

struct Edit {
    std::uint32_t offset{0};
    std::uint32_t length{0};
    std::string text;
};

std::string spell(const std::string& path, bool wasQuoted) {
    return wasQuoted ? format::quoteAlways(path) : format::quoteIfNeeded(path);
}

std::string readAll(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

/// A file name for the bundle's samples/ folder: the original's, with spaces made
/// underscores so it can be written bare even in a v1 file, whose CLIP lines cannot be
/// quoted.
std::string bundleName(const fs::path& source) {
    std::string name = source.filename().string();
    std::ranges::replace(name, ' ', '_');
    return name;
}

/// Where a voicebank folder is: the same places a sample is looked for, as a folder.
fs::path resolveFolder(const project::Resources& resources, std::span<const fs::path> library,
                       std::string_view written) {
    const fs::path path{std::string(written)};
    const fs::path base{resources.baseDirectory};
    std::vector<fs::path> candidates{path.is_absolute() ? path : base / path};
    for (const std::string& folder : resources.searchPaths) {
        candidates.push_back(base / folder / path.filename());
    }
    for (const fs::path& folder : library) {
        candidates.push_back(folder / path.filename());
    }
    for (const fs::path& candidate : candidates) {
        std::error_code error;
        if (fs::is_directory(candidate, error)) {
            return candidate;
        }
    }
    return {};
}

std::uint32_t crc32(std::span<const char> bytes, std::uint32_t crc = 0) {
    static const std::array<std::uint32_t, 256> kTable = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
            }
            t.at(i) = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const char byte : bytes) {
        crc = kTable.at((crc ^ static_cast<std::uint8_t>(byte)) & 0xFFU) ^ (crc >> 8U);
    }
    return ~crc;
}

void put(std::string& out, std::uint32_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out += static_cast<char>((value >> (8U * static_cast<unsigned>(i))) & 0xFFU);
    }
}

} // namespace

std::string
rewritePaths(std::string_view text,
             const std::function<std::optional<std::string>(std::string_view, PathKind)>& map) {
    format::DiagnosticList diagnostics;
    format::Document document = format::Document::parse(text, diagnostics);
    std::vector<Edit> edits;
    const auto edit = [&](const format::Span& span, const std::string& path, bool quoted,
                          PathKind kind) {
        if (const std::optional<std::string> replacement = map(path, kind)) {
            edits.push_back(Edit{.offset = span.byteOffset,
                                 .length = span.length,
                                 .text = spell(*replacement, quoted)});
        }
    };
    for (const format::Line& line : document.lines()) {
        if (line.kind == format::Line::Kind::KeyValue &&
            (line.key == "VOICEBANK" || line.key == "FROZEN")) {
            const bool quoted = !line.value.empty() && line.value.front() == '"';
            edit(line.valueSpan, format::decodeValue(line.value), quoted,
                 line.key == "VOICEBANK" ? PathKind::Folder : PathKind::Sample);
            continue;
        }
        if (line.kind != format::Line::Kind::Positional) {
            continue;
        }
        const std::vector<format::Token> tokens = format::tokenize(line, diagnostics);
        if (tokens.size() < 2) {
            continue;
        }
        const std::string& verb = tokens.front().text;
        if (verb == "AUDIO" || verb == "CLIP" || verb == "ZONE") {
            edit(tokens[1].span, tokens[1].text, tokens[1].quoted, PathKind::Sample);
        } else if (verb == "SLOT") {
            for (const format::Token& token : tokens) {
                if (token.isKeyValue() && token.text == "ir") {
                    edit(token.valueSpan, token.value, token.valueQuoted, PathKind::Sample);
                }
            }
        }
    }
    std::string out(text);
    std::ranges::sort(edits, [](const Edit& a, const Edit& b) { return a.offset > b.offset; });
    for (const Edit& e : edits) {
        out.replace(e.offset, e.length, e.text);
    }
    return out;
}

BundleResult bundleProject(const BundleOptions& options) {
    BundleResult result;
    const std::string text = readAll(options.project);
    if (text.empty()) {
        result.error = "could not read " + options.project.string();
        return result;
    }
    // Loaded for its resources: where its relative paths are relative to, and its own
    // SAMPLE_PATHS.
    project::Project project;
    project::CommandStack stack;
    format::DiagnosticList diagnostics;
    format::load(text, project, stack, diagnostics,
                 format::ParseOptions{.baseDirectory = options.project.parent_path().string()});

    std::error_code fsError;
    const fs::path samples = options.destination / "samples";
    fs::create_directories(samples, fsError);
    if (fsError) {
        result.error = "could not create " + samples.string() + ": " + fsError.message();
        return result;
    }
    std::map<std::string, std::optional<std::string>> decided;
    std::map<std::string, fs::path> taken; // bundle name -> the source it holds
    const auto place = [&](std::string_view written, PathKind kind) -> std::optional<std::string> {
        const std::string key{written};
        if (const auto found = decided.find(key); found != decided.end()) {
            return found->second;
        }
        const fs::path source =
            kind == PathKind::Folder
                ? resolveFolder(project.resources, options.library, written)
                : format::resolveSamplePath(project.resources, options.library, written);
        const bool exists =
            kind == PathKind::Folder ? !source.empty() : format::sampleExists(source);
        if (!exists) {
            result.missing.push_back(key);
            decided[key] = std::nullopt;
            return std::nullopt;
        }
        // The same name from two different places becomes name_2, name_3...
        std::string name = bundleName(source);
        const fs::path canonical = fs::weakly_canonical(source, fsError);
        for (int n = 2; taken.contains(name) && taken[name] != canonical; ++n) {
            name = fs::path(bundleName(source)).stem().string() + "_" + std::to_string(n) +
                   fs::path(bundleName(source)).extension().string();
        }
        if (!taken.contains(name)) {
            taken[name] = canonical;
            const fs::path target = samples / name;
            if (kind == PathKind::Folder) {
                fs::copy(source, target,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                         fsError);
            } else {
                fs::copy_file(source, target, fs::copy_options::overwrite_existing, fsError);
            }
            if (fsError) {
                result.error = "could not copy " + source.string() + ": " + fsError.message();
                return std::nullopt;
            }
            ++result.filesCopied;
        }
        std::string relative = "samples/" + name;
        result.rewritten.emplace_back(key, relative);
        decided[key] = relative;
        return relative;
    };
    const std::string rewritten = rewritePaths(text, place);
    if (!result.error.empty()) {
        return result;
    }
    result.projectOut = options.destination / options.project.filename();
    {
        std::ofstream out(result.projectOut, std::ios::binary);
        out.write(rewritten.data(), static_cast<std::streamsize>(rewritten.size()));
        if (!out) {
            result.error = "could not write " + result.projectOut.string();
            return result;
        }
    }
    if (options.zip) {
        result.zipOut =
            options.destination.parent_path() / (options.destination.filename().string() + ".zip");
        if (!zipFolder(options.destination, result.zipOut, result.error)) {
            return result;
        }
    }
    result.ok = true;
    return result;
}

bool zipFolder(const fs::path& folder, const fs::path& zip, std::string& error) {
    std::vector<fs::path> files;
    std::error_code fsError;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(folder, fsError)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files); // the same archive for the same folder, byte for byte
    std::FILE* out = openForWriting(zip);
    if (out == nullptr) {
        error = "could not create " + zip.string();
        return false;
    }
    std::string central;
    std::uint64_t offset = 0;
    bool ok = true;
    for (const fs::path& file : files) {
        const std::string data = readAll(file);
        const std::string name = fs::relative(file, folder.parent_path()).generic_string();
        if (data.size() >= 0xFFFFFFFFULL || offset >= 0xFFFFFFFFULL) {
            error = "a zip without ZIP64 holds files under 4 GiB: " + file.string();
            ok = false;
            break;
        }
        const std::uint32_t crc = crc32(data);
        // Version 2.0, UTF-8 names, stored, and a fixed 1980-01-01 timestamp so the
        // archive is a function of the files alone.
        std::string header;
        put(header, 0x04034B50, 4);
        put(header, 20, 2);
        put(header, 0x0800, 2);
        put(header, 0, 2);
        put(header, 0, 2);
        put(header, 0x0021, 2);
        put(header, crc, 4);
        put(header, static_cast<std::uint32_t>(data.size()), 4);
        put(header, static_cast<std::uint32_t>(data.size()), 4);
        put(header, static_cast<std::uint32_t>(name.size()), 2);
        put(header, 0, 2);
        header += name;
        std::string entry;
        put(entry, 0x02014B50, 4);
        put(entry, 20, 2);
        put(entry, 20, 2);
        put(entry, 0x0800, 2);
        put(entry, 0, 2);
        put(entry, 0, 2);
        put(entry, 0x0021, 2);
        put(entry, crc, 4);
        put(entry, static_cast<std::uint32_t>(data.size()), 4);
        put(entry, static_cast<std::uint32_t>(data.size()), 4);
        put(entry, static_cast<std::uint32_t>(name.size()), 2);
        put(entry, 0, 2);
        put(entry, 0, 2);
        put(entry, 0, 2);
        put(entry, 0, 2);
        put(entry, 0, 4);
        put(entry, static_cast<std::uint32_t>(offset), 4);
        entry += name;
        central += entry;
        ok = std::fwrite(header.data(), 1, header.size(), out) == header.size() &&
             std::fwrite(data.data(), 1, data.size(), out) == data.size();
        if (!ok) {
            error = "write failed (disk full?)";
            break;
        }
        offset += header.size() + data.size();
    }
    if (ok) {
        std::string end;
        put(end, 0x06054B50, 4);
        put(end, 0, 2);
        put(end, 0, 2);
        put(end, static_cast<std::uint32_t>(files.size()), 2);
        put(end, static_cast<std::uint32_t>(files.size()), 2);
        put(end, static_cast<std::uint32_t>(central.size()), 4);
        put(end, static_cast<std::uint32_t>(offset), 4);
        put(end, 0, 2);
        ok = std::fwrite(central.data(), 1, central.size(), out) == central.size() &&
             std::fwrite(end.data(), 1, end.size(), out) == end.size();
    }
    ok = (std::fclose(out) == 0) && ok;
    if (!ok && error.empty()) {
        error = "could not finish " + zip.string();
    }
    return ok;
}

} // namespace adx::render
