// The one Python-visible owner of a project.
//
// A Project and the CommandStack that mutates it are only meaningful together - an
// edit that bypassed the stack would be an edit undo cannot see - so Python never
// holds one without the other.
#pragma once

#include <string>

#include "engine/format/adx/Diagnostics.h"
#include "engine/project/Project.h"
#include "engine/project/commands/CommandStack.h"

namespace adx::bindings {

struct ProjectHandle {
    project::Project project;
    project::CommandStack stack;
    /// How the file was spelled on disk, so saving it back does not churn its line
    /// endings or gain a byte order mark it never had.
    std::string lineEnding{"\n"};
    bool bom{false};
    /// The version the file was read as. 1 means it went through the v1 shim.
    int sourceVersion{project::kAdxVersion};
};

/// Parses `text` into a fresh handle. The load is one undo step, and the history is
/// then cleared: loading a file is not an edit the user wants to undo past.
void loadInto(ProjectHandle& handle, std::string_view text, const std::string& baseDirectory,
              format::DiagnosticList& diagnostics);

} // namespace adx::bindings
