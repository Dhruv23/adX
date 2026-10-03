// adx-thread: main
#include "engine/preset/PresetLibrary.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace adx::preset {
namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// A file name that every filesystem accepts, from a preset name.
std::string fileNameFor(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const bool safe =
            std::isalnum(static_cast<unsigned char>(c)) != 0 || c == ' ' || c == '-' || c == '_';
        out += safe ? c : '_';
    }
    return out.empty() ? std::string("preset") : out;
}

} // namespace

std::size_t PresetLibrary::scan(const std::filesystem::path& directory,
                                format::DiagnosticList& diagnostics, bool user) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return 0;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error)) {
        if (entry.is_regular_file() && entry.path().extension() == kPresetExtension) {
            files.push_back(entry.path());
        }
    }
    // Sorted, so two scans of one tree list presets in one order on every platform.
    std::ranges::sort(files);
    std::size_t added = 0;
    for (const std::filesystem::path& file : files) {
        Preset preset;
        format::DiagnosticList local;
        if (!loadPreset(file, preset, local)) {
            continue;
        }
        for (const format::Diagnostic& diagnostic : local.all()) {
            diagnostics.add(diagnostic.code, diagnostic.span,
                            file.filename().string() + ": " + diagnostic.message, diagnostic.hint);
        }
        add(std::move(preset), user);
        ++added;
    }
    return added;
}

void PresetLibrary::add(Preset preset, bool user) {
    for (std::size_t i = 0; i < m_presets.size(); ++i) {
        if (m_presets[i].name == preset.name && m_presets[i].pack == preset.pack) {
            // Same name in the same pack: the user's copy wins, or a rescan replaces.
            if (user || !m_user[i]) {
                m_presets[i] = std::move(preset);
                m_user[i] = user;
            }
            return;
        }
    }
    m_presets.push_back(std::move(preset));
    m_user.push_back(user);
}

bool PresetLibrary::save(Preset preset) {
    if (m_userDirectory.empty()) {
        return false;
    }
    const std::filesystem::path path = m_userDirectory /
                                       (preset.pack.empty() ? std::string("user") : preset.pack) /
                                       (fileNameFor(preset.name) + std::string(kPresetExtension));
    if (!savePreset(preset, path)) {
        return false;
    }
    preset.source = path;
    add(std::move(preset), true);
    return true;
}

const Preset* PresetLibrary::find(std::string_view name, std::string_view pack) const noexcept {
    for (const Preset& preset : m_presets) {
        if (preset.name == name && (pack.empty() || preset.pack == pack)) {
            return &preset;
        }
    }
    return nullptr;
}

std::vector<const Preset*> PresetLibrary::withTag(std::string_view tag) const {
    std::vector<const Preset*> out;
    for (const Preset& preset : m_presets) {
        if (preset.hasTag(tag)) {
            out.push_back(&preset);
        }
    }
    return out;
}

std::vector<const Preset*> PresetLibrary::inPack(std::string_view pack) const {
    std::vector<const Preset*> out;
    for (const Preset& preset : m_presets) {
        if (preset.pack == pack) {
            out.push_back(&preset);
        }
    }
    return out;
}

std::vector<const Preset*> PresetLibrary::ofType(std::string_view type) const {
    std::vector<const Preset*> out;
    for (const Preset& preset : m_presets) {
        if (preset.type == type) {
            out.push_back(&preset);
        }
    }
    return out;
}

std::vector<const Preset*> PresetLibrary::search(std::string_view query) const {
    std::vector<std::string> words;
    std::string current;
    for (const char c : lower(query)) {
        if (c == ' ') {
            if (!current.empty()) {
                words.push_back(std::move(current));
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }

    std::vector<const Preset*> out;
    for (const Preset& preset : m_presets) {
        std::vector<std::string> fields{lower(preset.name), lower(preset.type), lower(preset.pack)};
        for (const std::string& tag : preset.tags) {
            fields.push_back(lower(tag));
        }
        const bool all = std::ranges::all_of(words, [&](const std::string& word) {
            return std::ranges::any_of(fields, [&](const std::string& field) {
                return field.find(word) != std::string::npos;
            });
        });
        if (all) {
            out.push_back(&preset);
        }
    }
    std::ranges::sort(out, {}, [](const Preset* p) { return p->name; });
    return out;
}

std::vector<std::string> PresetLibrary::tags() const {
    std::set<std::string> unique;
    for (const Preset& preset : m_presets) {
        unique.insert(preset.tags.begin(), preset.tags.end());
    }
    return {unique.begin(), unique.end()};
}

std::filesystem::path packsDirectory(const std::filesystem::path& repoRoot) {
    return repoRoot / "engine" / "preset" / "packs";
}

} // namespace adx::preset
