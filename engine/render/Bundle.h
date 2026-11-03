// adx-thread: main
//
// Collect-and-save (phase_8.md §3.7, FINAL_PLAN §5.5): every file a project refers to
// - audio clips, sampler zones, convolution impulses, voicebanks - copied into the
// bundle's `samples/` folder, the paths in the `.adx` rewritten to point there, and,
// on request, the folder zipped.
//
// The rewrite edits the text, not the model. Only the spans that spell a path change;
// every other byte of the file - comments, alignment, line endings, keys this version
// does not know - is copied as it was (bundle_preserves_formatting). A bundle is a
// normal folder with a normal text file in it, and stays one.
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace adx::render {

struct BundleOptions {
    /// The project to collect.
    std::filesystem::path project;
    /// The bundle folder, created if needed. The project is written into it under its
    /// own file name, the files into its `samples/`.
    std::filesystem::path destination;
    /// Also write `<destination>.zip` beside the folder.
    bool zip{false};
    /// Library folders to look in for a file that is not where its path says (the
    /// app's preferences), after the project's own SAMPLE_PATHS.
    std::vector<std::filesystem::path> library;
};

struct BundleResult {
    bool ok{false};
    std::string error;
    std::filesystem::path projectOut;
    std::filesystem::path zipOut;
    /// Every path rewritten, as written before and after.
    std::vector<std::pair<std::string, std::string>> rewritten;
    /// Paths whose files could not be found: left as written, reported here.
    std::vector<std::string> missing;
    std::size_t filesCopied{0};
};

[[nodiscard]] BundleResult bundleProject(const BundleOptions& options);

/// What kind of file a path in the text names.
enum class PathKind : std::uint8_t { Sample, Folder };

/// `text` with each path it refers to replaced by `map(path, kind)` where that returns a
/// value. Paths are found where the format puts them: AUDIO and v1 CLIP items, ZONE
/// lines, VOICEBANK, FROZEN, and a slot's `ir=`. A quoted path stays quoted; a bare one is
/// quoted only if its replacement needs it.
[[nodiscard]] std::string
rewritePaths(std::string_view text,
             const std::function<std::optional<std::string>(std::string_view, PathKind)>& map);

/// A store-only zip of every file under `folder`, paths relative to its parent. No
/// compression - audio barely compresses - and so no dependency: a CRC and two headers
/// per file.
[[nodiscard]] bool zipFolder(const std::filesystem::path& folder, const std::filesystem::path& zip,
                             std::string& error);

} // namespace adx::render
