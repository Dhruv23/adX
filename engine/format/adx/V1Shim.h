// Reading a v1 file into the v2 model.
//
// Written last, because it is defined entirely in terms of v2. The mapping is
// normative in docs/adx-format-v2.md §12 and was derived by reading
// `_archive/src-cpp/src/AdxParser.cpp`, not v1's documentation, which is known to
// be incomplete.
//
// Two rules govern everything here:
//
//   1. **Migration is in-memory only.** A v1 file is never rewritten in place.
//      `adx fmt --upgrade in.adx -o out.adx` is the explicit, opt-in path.
//   2. **Every decision that loses or invents information emits an ADX4xxx.**
//      Loading suffocation.adx should produce a readable migration report, not
//      silence.
//
// The centrepiece is the 1-to-4 expansion: one v1 `[TRACK]` becomes a Channel, a
// Pattern, a PlaylistTrack and an Insert. That expansion *is* the fix for the flat
// `Track` that blocked every DAW feature (FINAL_PLAN §3.3.7).
#pragma once

#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Document.h"
#include "engine/format/adx/Parser.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::format {

/// Builds a v2 project from a v1 document, through the same command set everything
/// else uses. `document` is non-const because the shim marks the lines it
/// understood; anything left over becomes residue and an ADX4012.
void migrateV1(Document& document, project::Project& project, project::CommandStack& stack,
               DiagnosticList& diagnostics, const ParseOptions& options = {});

} // namespace adx::format
