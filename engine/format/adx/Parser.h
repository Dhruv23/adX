// Document to Project.
//
// The parser mutates the project **through commands**, without exception. That is
// not a stylistic choice: FINAL_PLAN §4 guarantees that the writer, the GUI and the
// Python API can never drift apart because they share one command set, and a parser
// that reached into the model directly would be the one path that could. Loading a
// file is therefore undoable, which falls out for free and is what
// `parser_uses_commands` asserts.
//
// Sections are processed in a fixed internal order rather than in file order, so
// forward references resolve without a second pass: meter, tempo, project,
// inserts, slots, sends, routes, channels, channel outputs, patterns, pattern
// bodies, playlist, markers. A file may list its sections in any order.
#pragma once

#include <string>
#include <string_view>

#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Document.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::format {

struct ParseOptions {
    /// Directory the file was loaded from, used to resolve relative sample paths.
    /// Not serialised - it is a fact about where the file is, not about the project.
    std::string baseDirectory;

    /// Collect the whole load into one undo step. On by default: loading a 100k-note
    /// file must not put 100k entries in the history panel, and one group is also
    /// what keeps the load under its half-second budget.
    bool groupAsOneStep{true};
};

/// True when the document declares ADX_VERSION >= 2. Everything else goes through
/// the v1 shim - including a file that has no [PROJECT] section at all, which is
/// what every v1 file looks like.
[[nodiscard]] bool isVersion2(const Document& document) noexcept;

/// Parses a v2 document. The document is non-const because the parser marks the
/// lines it understood; whatever is left becomes residue.
void parseV2(Document& document, project::Project& project, project::CommandStack& stack,
             DiagnosticList& diagnostics, const ParseOptions& options = {});

/// Reads text of either version into `project`, dispatching on the version.
///
/// `project` must be empty. `stack` receives the commands, so undoing them all
/// returns the project to empty.
void load(std::string_view text, project::Project& project, project::CommandStack& stack,
          DiagnosticList& diagnostics, const ParseOptions& options = {});

} // namespace adx::format
