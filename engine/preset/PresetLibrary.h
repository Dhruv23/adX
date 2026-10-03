// adx-thread: main
//
// The preset library: ported from _archive/src-cpp/src/PatchLibrary.cpp and retargeted
// at the v2 preset format (phase_4.md §4.12, §5).
//
// The archive's library was nine patches compiled into the binary. This one scans
// directories of `.adxpreset` files - the shipped packs and the user's own - indexes
// them by name, tag and pack, searches them, and saves the user's presets to a user
// directory kept apart from the packs, so updating adX never overwrites a user's work
// and a user's preset never shadows a shipped one silently.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/format/adx/Diagnostics.h"
#include "engine/preset/Preset.h"

namespace adx::preset {

inline constexpr std::string_view kPresetExtension = ".adxpreset";

class PresetLibrary {
public:
    /// Loads every `.adxpreset` under `directory`, recursively. A preset whose file
    /// cannot be read is skipped; one that parses with diagnostics is kept, and the
    /// diagnostics are appended to `diagnostics` (prefixed with the file). Returns how
    /// many were added. `user` marks them as the user's, which save() writes beside.
    std::size_t scan(const std::filesystem::path& directory, format::DiagnosticList& diagnostics,
                     bool user = false);

    /// The directory save() writes to. Never a pack directory.
    void setUserDirectory(std::filesystem::path directory) {
        m_userDirectory = std::move(directory);
    }
    [[nodiscard]] const std::filesystem::path& userDirectory() const noexcept {
        return m_userDirectory;
    }

    /// Writes `preset` into the user directory, as <pack or "user">/<name>.adxpreset,
    /// and adds or replaces it in the library. False without a user directory or on
    /// a write failure.
    bool save(Preset preset);

    [[nodiscard]] const std::vector<Preset>& all() const noexcept {
        return m_presets;
    }
    /// Exact name, and pack when given. The user's copy wins over a pack's of the same
    /// name and pack, which is the only shadowing there is.
    [[nodiscard]] const Preset* find(std::string_view name,
                                     std::string_view pack = {}) const noexcept;
    [[nodiscard]] std::vector<const Preset*> withTag(std::string_view tag) const;
    [[nodiscard]] std::vector<const Preset*> inPack(std::string_view pack) const;
    [[nodiscard]] std::vector<const Preset*> ofType(std::string_view type) const;
    /// Case-insensitive substring of name, type, pack or any tag; every word of
    /// `query` must match one of them. Results in name order.
    [[nodiscard]] std::vector<const Preset*> search(std::string_view query) const;

    /// Every tag in use, sorted, once each - what a browser's tag filter lists.
    [[nodiscard]] std::vector<std::string> tags() const;

private:
    void add(Preset preset, bool user);

    std::vector<Preset> m_presets;
    std::vector<bool> m_user;
    std::filesystem::path m_userDirectory;
};

/// Where the shipped packs live in a source tree: engine/preset/packs.
[[nodiscard]] std::filesystem::path packsDirectory(const std::filesystem::path& repoRoot);

} // namespace adx::preset
