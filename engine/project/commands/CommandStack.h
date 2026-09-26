// Undo, redo, grouping and coalescing.
//
// The coalescing rule is written down here rather than guessed at, because "why did
// my undo eat the whole drag" is otherwise unanswerable. `execute()` offers the new
// command to the one on top of the stack only when **all** of these hold:
//
//   - same CommandId,
//   - same targetKey(),
//   - no beginGroup boundary between them,
//   - less than kCoalesceWindow since the previous execute.
//
// A note drag becomes one history entry. A drag, a pause, and another drag become
// two (phase_2.md §4.9).
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "engine/project/commands/Command.h"

namespace adx::project {

class Project;

/// How long two same-kind edits may be apart and still merge.
inline constexpr std::chrono::milliseconds kCoalesceWindow{500};

struct HistoryEntry {
    std::string label;
    CommandId kind{CommandId::kGroup};
    DirtyMask dirty{dirty::kNone};

    [[nodiscard]] friend bool operator==(const HistoryEntry&,
                                         const HistoryEntry&) noexcept = default;
};

class CommandStack {
public:
    using Clock = std::chrono::steady_clock;

    CommandStack() = default;
    ~CommandStack() = default;

    CommandStack(const CommandStack&) = delete;
    CommandStack& operator=(const CommandStack&) = delete;
    CommandStack(CommandStack&&) = default;
    CommandStack& operator=(CommandStack&&) = default;

    /// Applies, pushes, and clears the redo stack.
    void execute(std::unique_ptr<Command> command, Project& project);

    /// The same, with the clock supplied.
    ///
    /// Not a testing back door but the honest shape of the thing: coalescing is a
    /// function of when the edits happened, so a test that asserts on the window has
    /// to be able to say when. `execute()` is this with Clock::now().
    void executeAt(std::unique_ptr<Command> command, Project& project, Clock::time_point now);

    [[nodiscard]] bool undo(Project& project);
    [[nodiscard]] bool redo(Project& project);

    /// Starts an explicit transaction. Everything executed until the matching
    /// endGroup() becomes one history entry and one undo step. Nesting is counted,
    /// and only the outermost pair forms the entry.
    void beginGroup(std::string_view label);
    void endGroup();
    [[nodiscard]] bool inGroup() const noexcept {
        return m_groupDepth > 0;
    }

    [[nodiscard]] std::span<const HistoryEntry> history() const noexcept {
        return m_history;
    }

    /// Bumped on every mutation, including undo and redo.
    ///
    /// This is the change token the UI and Phase 3 both poll. It is cheaper and more
    /// reliable than a signal graph: a poller cannot miss an edit, and it cannot be
    /// woken by an edit that did not happen.
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return m_revision;
    }

    /// What has changed since `revision`, for an incremental snapshot rebuild.
    /// Conservatively kAll when the caller is further behind than the stack can
    /// account for.
    [[nodiscard]] DirtyMask dirtySince(std::uint64_t revision) const noexcept;

    [[nodiscard]] std::size_t undoDepth() const noexcept {
        return m_done.size();
    }
    [[nodiscard]] std::size_t redoDepth() const noexcept {
        return m_undone.size();
    }
    [[nodiscard]] bool canUndo() const noexcept {
        return !m_done.empty();
    }
    [[nodiscard]] bool canRedo() const noexcept {
        return !m_undone.empty();
    }

    /// Forgets the history without touching the project. Used after a load, when the
    /// commands that built the project are not edits the user wants to undo past.
    void clear() noexcept;

private:
    void push(std::unique_ptr<Command> command, Clock::time_point now);
    void recordDirty(DirtyMask mask);

    std::vector<std::unique_ptr<Command>> m_done;
    std::vector<std::unique_ptr<Command>> m_undone;
    std::vector<HistoryEntry> m_history;

    /// The group currently being collected, and how deep the nesting is.
    std::vector<std::unique_ptr<Command>> m_group;
    std::string m_groupLabel;
    int m_groupDepth{0};

    Clock::time_point m_lastExecute{};
    bool m_hasLastExecute{false};
    /// Set by beginGroup/endGroup so a command executed just after a transaction
    /// cannot merge into the one just before it.
    bool m_coalesceBarrier{true};

    std::uint64_t m_revision{0};
    /// dirty mask per revision, oldest first, paired with the revision it produced.
    std::vector<std::pair<std::uint64_t, DirtyMask>> m_dirtyLog;
};

} // namespace adx::project
