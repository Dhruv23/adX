// adx-thread: main
//
// The bidirectional coordinator (phase_7.md §4.1): one project, its file, and the
// three ways they change.
//
//   model -> text            a command ran: renderText() is the canonical text
//   text (in-app) -> model   the editor panel's buffer changed: applyText()
//   text (external) -> model the watcher fired: applyExternal()
//
// All three meet at the Document and all three end in commands - Patch.h - so undo
// keeps working, the GUI updates, and a text edit and a knob turn are the same
// operation, not two code paths that agree by luck.
//
// Loop prevention, three guards, any one of which breaks the loop:
//
//   1. **Origin tagging.** Every write adX makes is recorded by its content hash
//      (noteWrite); a watcher event whose file hashes to one of them is adX hearing
//      itself, and is ignored (ApplyStatus::SelfWrite).
//   2. **No-op diffing.** A patch with no difference executes nothing, so the
//      revision does not move and nothing downstream wakes.
//   3. **Re-entrancy.** Applying is not re-entrant. If anything calls back into an
//      apply while one runs, that is a bug, and it is asserted rather than absorbed.
//
// Conflict: an external change while the model has edits the file has not seen
// (modelDirty), whose text does not already contain them, is not applied. It is
// reported - ApplyStatus::Conflict - and the caller offers the three resolutions:
// keep mine (write renderText() over the file), take theirs (applyForced()), or
// open a diff. Silently picking one is how a person loses work, and this project's
// premise is that people will edit the file in two places.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Patch.h"
#include "engine/format/adx/Writer.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::sync {

enum class SyncMode : std::uint8_t {
    /// The file is written on save only, and external changes are not watched.
    Off,
    /// The default: model -> text on save; text -> model when the file changes.
    OnSave,
    /// model -> text debounced after every edit; text -> model on every edit.
    Live,
};

enum class ApplyStatus : std::uint8_t {
    /// The text says what the model already says.
    Unchanged,
    /// The model was patched to the text.
    Applied,
    /// The text has errors; the model was left alone.
    Invalid,
    /// The model has edits the text does not; nothing was applied.
    Conflict,
    /// The text is something adX itself just wrote.
    SelfWrite,
};

[[nodiscard]] const char* toString(SyncMode mode) noexcept;
[[nodiscard]] const char* toString(ApplyStatus status) noexcept;

struct ApplyResult {
    ApplyStatus status{ApplyStatus::Unchanged};
    format::PatchResult patch;
    /// The text's diagnostics, whatever the status.
    format::DiagnosticList diagnostics;
};

class TextSync {
public:
    /// `spelling` is how the file writes its line endings and BOM; `baseDirectory`
    /// resolves its relative sample paths.
    TextSync(project::Project& project, project::CommandStack& stack, format::WriteOptions spelling,
             std::string baseDirectory);

    void setMode(SyncMode mode) noexcept {
        m_mode = mode;
    }
    [[nodiscard]] SyncMode mode() const noexcept {
        return m_mode;
    }

    /// The file and the model agree on `text`: after a load, a save, or a write.
    void markSynced(std::string_view text);

    /// The canonical text of the model, spelled as the file is.
    [[nodiscard]] std::string renderText() const;

    /// The editor panel's buffer. Errors leave the model alone.
    ApplyResult applyText(std::string_view text);

    /// The file changed on disk. A self-write is ignored, errors leave the model
    /// alone, and a change that would discard unsynced model edits is a Conflict.
    ApplyResult applyExternal(std::string_view text);

    /// "Take theirs": apply `text` even over unsynced model edits (which stay in the
    /// history, one undo away).
    ApplyResult applyForced(std::string_view text);

    /// Records a write adX made, so the watcher event it causes is recognised.
    void noteWrite(std::string_view text);

    /// The model has been edited since the last sync - cheap: a revision compare. It
    /// may still say what the synced text says (an edit and its undo, a value set to
    /// what it was); modelDiffers() is the exact question.
    [[nodiscard]] bool modelDirty() const noexcept;

    /// The model says something the last synced text does not. What a conflict is
    /// decided on: an edit that changed nothing is not a reason to refuse a reload.
    [[nodiscard]] bool modelDiffers() const;

    /// The text the file had when the two last agreed.
    [[nodiscard]] const std::string& syncedText() const noexcept {
        return m_syncedText;
    }

    /// Patches that changed the model, for tests and the status bar.
    [[nodiscard]] std::uint64_t applies() const noexcept {
        return m_applies;
    }

private:
    ApplyResult apply(std::string_view text, std::string_view label, bool external, bool force);

    project::Project& m_project;
    project::CommandStack& m_stack;
    format::WriteOptions m_spelling;
    std::string m_lineEnding;
    std::string m_baseDirectory;
    SyncMode m_mode{SyncMode::OnSave};

    std::string m_syncedText;
    std::uint64_t m_syncedRevision{0};
    /// Hashes of adX's recent writes, newest last. Bounded: a burst of writes the
    /// watcher coalesced only ever needs the last few.
    std::deque<std::uint64_t> m_recentWrites;
    bool m_applying{false};
    std::uint64_t m_applies{0};
};

/// 64-bit FNV-1a: what a write is recognised by.
[[nodiscard]] std::uint64_t contentHash(std::string_view text) noexcept;

} // namespace adx::sync
