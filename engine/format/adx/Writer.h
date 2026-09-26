// Project to text.
//
// The writer always emits the canonical form (docs/adx-format-v2.md §9), which is
// the same thing `adx fmt` produces. There is no "pretty" mode and no "compact"
// mode: two spellings of the same project would make every diff a negotiation
// about formatting rather than about music.
//
// The two properties this has to have, both of them property tests:
//
//   fmt(fmt(x)) == fmt(x)                 - idempotent
//   parse(write(p)) is p                  - lossless, residue included
#pragma once

#include <string>
#include <string_view>

namespace adx::project {
class Project;
}

namespace adx::format {

struct WriteOptions {
    /// "\n" or "\r\n". A file loaded from disk is written back with the endings it
    /// had; a new file gets LF.
    std::string_view lineEnding{"\n"};
    /// Re-emit the byte order mark the file arrived with. A file that did not have
    /// one never gains one.
    bool emitBom{false};
};

/// The canonical text for `project`.
[[nodiscard]] std::string write(const project::Project& project, const WriteOptions& options = {});

} // namespace adx::format
