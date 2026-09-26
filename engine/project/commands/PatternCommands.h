// Commands on patterns.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/Time.h"
#include "engine/project/Color.h"
#include "engine/project/Pattern.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

class AddPattern final : public Command {
public:
    /// The prototype's `id` is ignored, and so are the ids of any notes it carries -
    /// apply() assigns all of them, which is what lets a "duplicate pattern" command
    /// be written as a copy plus this.
    explicit AddPattern(Pattern prototype);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::PatternId created() const noexcept {
        return m_id;
    }

private:
    Pattern m_prototype;
    core::PatternId m_id;
    IdMarks m_marks;
};

/// Removes a pattern and every playlist item that placed it.
///
/// Leaving the items behind would give the playlist references to a pattern that no
/// longer exists, which Validate would then report on a project the user never made
/// invalid themselves.
class RemovePattern final : public Command {
public:
    explicit RemovePattern(core::PatternId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    struct DetachedItem {
        core::PlaylistTrackId track;
        std::size_t index{0};
        PlaylistItem item;
    };

    core::PatternId m_id;
    std::size_t m_index{0};
    Pattern m_previous;
    std::vector<DetachedItem> m_items;
    bool m_removed{false};
};

class RenamePattern final : public Command {
public:
    RenamePattern(core::PatternId id, std::string patternName);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PatternId m_id;
    std::string m_name;
    std::string m_previous;
};

class SetPatternLength final : public Command {
public:
    SetPatternLength(core::PatternId id, core::Ticks length);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PatternId m_id;
    core::Ticks m_length;
    core::Ticks m_previous;
};

class SetPatternColor final : public Command {
public:
    SetPatternColor(core::PatternId id, Color color);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PatternId m_id;
    Color m_color;
    Color m_previous;
};

/// Sets (or, with an empty body, clears) one channel's mini-notation in a pattern.
///
/// Phase 2 stores the source text and nothing more - compiling it is Phase 4's and
/// wiring it live is Phase 7's. Storing it as a command anyway means Phase 7's
/// live-reload arrives with undo already working.
class SetMiniNotation final : public Command {
public:
    SetMiniNotation(core::PatternId pattern, core::ChannelId channel,
                    std::vector<std::string> lines);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::PatternId m_pattern;
    core::ChannelId m_channel;
    std::vector<std::string> m_lines;
    std::vector<std::string> m_previous;
    std::size_t m_index{0};
    bool m_existed{false};
};

} // namespace adx::project
