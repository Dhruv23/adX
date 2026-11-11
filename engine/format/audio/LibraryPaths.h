// adx-thread: main
//
// `lib://<library>/<path>`: a sample in the library store (phase_12.md §4.7.3, §4.8).
//
// Libraries are data, not dependencies: VSCO 2 CE, VCSL and the rest are fetched by
// tools/packs/fetch_libraries.py into `<store>/<id>@<commit>`, and a zone names one of
// their files without knowing where the store is or which commit is installed. The
// store is %LOCALAPPDATA%\adX\libraries, or ADX_LIBRARY_STORE.
//
// A library that is not installed resolves to an empty path: the zone has no sample,
// the preset still loads, and the caller says so (preset::missingLibraries).
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace adx::format {

inline constexpr std::string_view kLibraryScheme = "lib://";

[[nodiscard]] inline bool isLibraryPath(std::string_view path) noexcept {
    return path.starts_with(kLibraryScheme);
}

/// The library a `lib://` path names; empty for any other path.
[[nodiscard]] std::string libraryOf(std::string_view path);

/// Where the store is: ADX_LIBRARY_STORE, else the per-user default.
[[nodiscard]] std::filesystem::path libraryStore();

/// The folder library `id` is installed in under `store`: `<id>` itself or, the fetch
/// tool's layout, the first `<id>@<commit>` by name. Empty when it is not installed.
[[nodiscard]] std::filesystem::path libraryFolder(const std::filesystem::path& store,
                                                  std::string_view id);

/// A `lib://` path as a file path in `store`; empty when the library is not installed.
/// The file itself is not checked: a missing file in an installed library is a missing
/// sample like any other.
[[nodiscard]] std::filesystem::path resolveLibraryPath(const std::filesystem::path& store,
                                                       std::string_view path);

/// The same in the user's store. Which folder each library is in is remembered, since
/// every zone of a preset asks on every snapshot; forgetLibraryFolders() makes the next
/// call look again (after a fetch, or when a test moves the store).
[[nodiscard]] std::filesystem::path resolveLibraryPath(std::string_view path);
void forgetLibraryFolders();

} // namespace adx::format
