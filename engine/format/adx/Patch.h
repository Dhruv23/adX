// adx-thread: main
//
// A re-parsed file, turned into the commands that bring the open project to it -
// the single most important algorithm of Phase 7 (phase_7.md §4.2), because it is
// what makes a reload *patch* rather than *rebuild*: FINAL_PLAN §3.2, "make reload
// diff-and-patch rather than rebuild-and-replace so playing voices are not cut".
//
// Three steps:
//
//   1. **Remap.** The new text is parsed into a project of its own, whose ids are
//      whatever the parser handed out. remapInto() rewrites them into the open
//      project's: entities match the way the file names them - channels, patterns
//      and controllers by name, inserts, slots, sends and playlist tracks by number,
//      samples by path - and notes by (start, pitch), so an edited velocity is a
//      modification of the note that is sounding, not a new note. Whatever does not
//      match gets a fresh id. After this, an entity that survived the edit has the
//      same id on both sides, and the engine's node store keys on ids: its node, its
//      voices and its reverb tail survive too.
//
//   2. **Narrow commands.** Where an entity differs only in values a command already
//      sets - a note's velocity, a channel's PARAM, an insert's gain, a pattern's
//      LENGTH, a MINI - the patch emits exactly that command, so the history panel
//      says "Set velocity" and `patch_minimal_catalogue` can count them.
//
//   3. **The remainder.** If the project still differs from the target after that, one
//      ReplaceProject (PatchCommands.h) sets the rest. Correctness never depends on
//      how much step 2 covers; only the size of the history entry does.
//
// Orders that mean nothing are not differences: sections may move in the file, and
// notes and markers are written sorted anyway. Any other order is written as it is,
// so it is content. So moving a section is zero commands, and
// a comment or a blank line is zero commands. A file a person reorders by hand is
// put back into canonical order on the next write - which is what canonical means.
//
// The whole patch is one beginGroup/endGroup, so an external reload is one undo step.
// phase_7.md §4.2 sketches computePatch() returning a command list; applying as it
// goes is simpler, and nothing needs the list (phase_7.md §10).
#pragma once

#include <cstddef>
#include <string_view>

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::format {

struct PatchResult {
    /// Commands executed, not counting the group around them. Zero: nothing changed,
    /// and nothing was added to the history.
    std::size_t commands{0};
    /// True when a ReplaceProject was needed for the remainder.
    bool structural{false};
};

/// Brings `current` to `target` - a fresh parse of the new text - through commands
/// executed on `stack`, as one history entry labelled `label`.
PatchResult applyPatch(project::Project& current, project::CommandStack& stack,
                       const project::Project& target, std::string_view label = "Reload from text");

/// `target` with its ids rewritten into `current`'s, as step 1 above describes. Its id
/// counters are `current`'s, advanced past every id it allocated or adopted.
[[nodiscard]] project::Project remapInto(const project::Project& current,
                                         const project::Project& target);

/// True when the two describe the same project: every value equal, with the orders that
/// carry no meaning ignored - whole sections, and what the writer sorts (notes in a
/// clip, markers) - and every order the writer preserves compared. Id counters and
/// where the residue was in its file are not content.
[[nodiscard]] bool sameContent(const project::Project& a, const project::Project& b);

} // namespace adx::format
