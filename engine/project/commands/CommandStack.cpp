#include "engine/project/commands/CommandStack.h"

#include <algorithm>
#include <ranges>
#include <utility>

#include "engine/core/Config.h"
#include "engine/project/Project.h"

namespace adx::project {
namespace {

/// How many revisions of dirty history to keep.
///
/// A poller that has fallen further behind than this gets kAll, which is correct
/// but slow - and a UI that has missed 256 edits has bigger problems than one extra
/// snapshot rebuild.
constexpr std::size_t kDirtyLogDepth = 256;

/// A transaction, as one command.
class GroupCommand final : public Command {
public:
    GroupCommand(std::string label, std::vector<std::unique_ptr<Command>> members)
        : m_label(std::move(label)), m_members(std::move(members)) {
        for (const auto& member : m_members) {
            m_dirty |= member->dirty();
        }
    }

    void apply(Project& project) override {
        for (const auto& member : m_members) {
            member->apply(project);
        }
    }

    void revert(Project& project) override {
        // Reverse order, which is the only order that works: a command that created
        // an entity must be reverted after everything that used it.
        for (const auto& member : std::views::reverse(m_members)) {
            member->revert(project);
        }
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return m_label;
    }
    [[nodiscard]] CommandId kind() const noexcept override {
        return CommandId::kGroup;
    }
    [[nodiscard]] DirtyMask dirty() const noexcept override {
        return m_dirty;
    }

private:
    std::string m_label;
    std::vector<std::unique_ptr<Command>> m_members;
    DirtyMask m_dirty{dirty::kNone};
};

} // namespace

void CommandStack::execute(std::unique_ptr<Command> command, Project& project) {
    executeAt(std::move(command), project, Clock::now());
}

void CommandStack::executeAt(std::unique_ptr<Command> command, Project& project,
                             Clock::time_point now) {
    if (command == nullptr) {
        return;
    }
    command->apply(project);
    const DirtyMask mask = command->dirty();

    if (m_groupDepth > 0) {
        m_group.push_back(std::move(command));
        recordDirty(mask);
        return;
    }

    push(std::move(command), now);
    recordDirty(mask);
}

void CommandStack::push(std::unique_ptr<Command> command, Clock::time_point now) {
    // A new edit invalidates everything that was undone. Doing this before the
    // coalescing attempt matters: merging into a command that has a redo stack
    // behind it would leave the redo stack describing a project that no longer
    // exists.
    m_undone.clear();

    const bool withinWindow =
        m_hasLastExecute && (now - m_lastExecute) < kCoalesceWindow && !m_coalesceBarrier;
    if (withinWindow && !m_done.empty()) {
        Command& top = *m_done.back();
        if (top.kind() == command->kind() && top.targetKey() == command->targetKey() &&
            top.coalesceWith(*command)) {
            m_history.back().dirty |= command->dirty();
            m_lastExecute = now;
            return;
        }
    }

    m_history.push_back(HistoryEntry{
        .label = std::string(command->name()), .kind = command->kind(), .dirty = command->dirty()});
    m_done.push_back(std::move(command));
    m_lastExecute = now;
    m_hasLastExecute = true;
    m_coalesceBarrier = false;
}

bool CommandStack::undo(Project& project) {
    ADX_ASSERT(m_groupDepth == 0);
    if (m_done.empty() || m_groupDepth > 0) {
        return false;
    }
    std::unique_ptr<Command> command = std::move(m_done.back());
    m_done.pop_back();
    const DirtyMask mask = command->dirty();
    command->revert(project);
    m_undone.push_back(std::move(command));
    m_history.pop_back();
    // An undo is an edit as far as coalescing is concerned: the next command must
    // start a fresh history entry rather than merging into whatever is now on top.
    m_coalesceBarrier = true;
    recordDirty(mask);
    return true;
}

bool CommandStack::redo(Project& project) {
    ADX_ASSERT(m_groupDepth == 0);
    if (m_undone.empty() || m_groupDepth > 0) {
        return false;
    }
    std::unique_ptr<Command> command = std::move(m_undone.back());
    m_undone.pop_back();
    const DirtyMask mask = command->dirty();
    command->apply(project);
    m_history.push_back(HistoryEntry{
        .label = std::string(command->name()), .kind = command->kind(), .dirty = mask});
    m_done.push_back(std::move(command));
    m_coalesceBarrier = true;
    recordDirty(mask);
    return true;
}

void CommandStack::beginGroup(std::string_view label) {
    if (m_groupDepth == 0) {
        m_groupLabel = std::string(label);
        m_group.clear();
    }
    ++m_groupDepth;
    m_coalesceBarrier = true;
}

void CommandStack::endGroup() {
    ADX_ASSERT(m_groupDepth > 0);
    if (m_groupDepth == 0) {
        return;
    }
    --m_groupDepth;
    if (m_groupDepth > 0) {
        return;
    }
    if (m_group.empty()) {
        m_groupLabel.clear();
        return;
    }
    // The members are already applied - executeAt applied each as it arrived - so
    // the group is pushed without being applied again.
    auto group = std::make_unique<GroupCommand>(std::move(m_groupLabel), std::move(m_group));
    m_group.clear();
    m_groupLabel.clear();
    m_undone.clear();
    m_history.push_back(HistoryEntry{
        .label = std::string(group->name()), .kind = CommandId::kGroup, .dirty = group->dirty()});
    m_done.push_back(std::move(group));
    m_coalesceBarrier = true;
}

void CommandStack::recordDirty(DirtyMask mask) {
    ++m_revision;
    m_dirtyLog.emplace_back(m_revision, mask);
    if (m_dirtyLog.size() > kDirtyLogDepth) {
        m_dirtyLog.erase(m_dirtyLog.begin());
    }
}

DirtyMask CommandStack::dirtySince(std::uint64_t revision) const noexcept {
    if (revision >= m_revision) {
        return dirty::kNone;
    }
    if (m_dirtyLog.empty() || m_dirtyLog.front().first > revision + 1) {
        return dirty::kAll;
    }
    DirtyMask mask = dirty::kNone;
    for (const auto& [at, bits] : m_dirtyLog) {
        if (at > revision) {
            mask |= bits;
        }
    }
    return mask;
}

void CommandStack::clear() noexcept {
    m_done.clear();
    m_undone.clear();
    m_history.clear();
    m_group.clear();
    m_groupLabel.clear();
    m_groupDepth = 0;
    m_hasLastExecute = false;
    m_coalesceBarrier = true;
}

} // namespace adx::project
