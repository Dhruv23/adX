// The invariant checker.
//
// Two callers, and the second one is the reason this exists as a separate pass
// rather than as scattered asserts: the loader runs it so a malformed file is
// reported rather than half-loaded, and Phase 3 runs it before snapshotting so the
// render path never has to defend against a model that cannot happen
// (phase_2.md §8).
//
// It never mutates. A project that fails validation is still a project; deciding
// what to do about it belongs to the caller.
#pragma once

#include <string>

#include "engine/format/adx/Diagnostics.h"

namespace adx::project {

class Project;

struct ValidationOptions {
    /// Check that every sample path exists on disk, relative to the project's
    /// directory. Off by default because it touches the filesystem, and a project
    /// being edited on a machine without its audio files is a warning, not a reason
    /// for the model layer to do I/O on every check.
    bool checkSampleFiles{false};
};

/// A span meaning "this diagnostic has no position in any file".
///
/// Validation runs on a Project, which may never have come from text - a project
/// built through the scripting API has no line numbers to report. Line zero is the
/// marker, and the CLI prints such diagnostics without a `file:line:col` prefix.
inline constexpr format::Span kNoSpan{.line = 0, .column = 0, .length = 0, .byteOffset = 0};

/// Appends a diagnostic for every broken invariant. Returns false if any of them is
/// an Error.
[[nodiscard]] bool validate(const Project& project, format::DiagnosticList& diagnostics,
                            const ValidationOptions& options = {});

/// True when `name` is usable as an entity name: non-empty, and free of the
/// characters that would make a reference to it ambiguous.
[[nodiscard]] bool isValidEntityName(std::string_view name) noexcept;

/// Makes `name` valid by replacing forbidden characters, and non-empty by
/// substituting `fallback`. Used by the v1 shim, which inherits whatever v1 allowed.
[[nodiscard]] std::string sanitizeEntityName(std::string_view name, std::string_view fallback);

} // namespace adx::project
