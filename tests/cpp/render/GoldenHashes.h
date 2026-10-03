// Golden hash files: `name hash` lines under a `#` header, in tests/golden/.
//
// One mechanism for every golden test, so regenerating is the same one command
// everywhere: run with ADX_UPDATE_GOLDEN=1 and say why in the commit.
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace adx::tests {

[[nodiscard]] std::filesystem::path goldenDirectory();

[[nodiscard]] std::map<std::string, std::string> readHashes(const std::filesystem::path& file);

/// Under ADX_UPDATE_GOLDEN, rewrites `file` with `header` and `actual`. Otherwise checks
/// that every entry of `actual` is committed in `file` with the same hash, and that
/// `file` holds nothing `actual` lacks (a removed entry is a change too).
void checkGolden(const std::filesystem::path& file, std::string_view header,
                 const std::map<std::string, std::string>& actual);

} // namespace adx::tests
