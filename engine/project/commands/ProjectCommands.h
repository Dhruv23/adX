// Commands that act on the project as a whole: metadata, tempo, meter, markers and
// the sample pool.
//
// Every create command here follows the same two-line discipline, and it is the
// reason `undo_redo_symmetry` passes on *text* rather than merely on structure:
// save `Project::idMarks()` before allocating, restore it in revert(). Redo then
// hands out the same id it handed out the first time, so the `insert.N` references
// the writer emits do not shift under an undo.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/core/TempoMap.h"
#include "engine/core/Time.h"
#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::project {

/// Replaces the whole ProjectMeta.
///
/// One command rather than one per field, because the fields are set together (by
/// the parser, from the [PROJECT] section) and edited rarely and individually (by a
/// properties dialog, which can afford a whole-struct command). A per-field command
/// set here would be seven classes that never coalesce with each other anyway.
class SetMeta final : public Command {
public:
    explicit SetMeta(ProjectMeta meta);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    ProjectMeta m_next;
    ProjectMeta m_previous;
};

/// Inserts or replaces a tempo event. Replacing is how an existing event's bpm is
/// changed, so there is no separate "edit" command.
class SetTempoEvent final : public Command {
public:
    SetTempoEvent(core::Ticks at, double bpm, bool ramp);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::Ticks m_at;
    double m_bpm{120.0};
    bool m_ramp{false};
    bool m_hadPrevious{false};
    core::TempoEvent m_previous;
};

class RemoveTempoEvent final : public Command {
public:
    explicit RemoveTempoEvent(core::Ticks at);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::Ticks m_at;
    bool m_removed{false};
    core::TempoEvent m_previous;
};

class SetMeterEvent final : public Command {
public:
    SetMeterEvent(core::Ticks at, std::uint16_t numerator, std::uint16_t denominator);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;

private:
    core::Ticks m_at;
    std::uint16_t m_numerator{4};
    std::uint16_t m_denominator{4};
    bool m_hadPrevious{false};
    core::MeterEvent m_previous;
};

class RemoveMeterEvent final : public Command {
public:
    explicit RemoveMeterEvent(core::Ticks at);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::Ticks m_at;
    bool m_removed{false};
    core::MeterEvent m_previous;
};

class AddMarker final : public Command {
public:
    AddMarker(core::Ticks at, std::string markerName);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::MarkerId created() const noexcept {
        return m_id;
    }

private:
    core::Ticks m_at;
    std::string m_name;
    core::MarkerId m_id;
    IdMarks m_marks;
};

class RemoveMarker final : public Command {
public:
    explicit RemoveMarker(core::MarkerId id);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

private:
    core::MarkerId m_id;
    std::size_t m_index{0};
    Marker m_previous;
    bool m_removed{false};
};

/// Moves and/or renames a marker. Coalesces, because dragging one along the ruler
/// should be one undo step.
class MoveMarker final : public Command {
public:
    MoveMarker(core::MarkerId id, core::Ticks at, std::string markerName);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;
    [[nodiscard]] std::uint64_t targetKey() const noexcept override;
    [[nodiscard]] bool coalesceWith(const Command& next) override;

private:
    core::MarkerId m_id;
    core::Ticks m_at;
    std::string m_name;
    core::Ticks m_previousAt;
    std::string m_previousName;
};

/// Adds a sample to the pool, or returns the existing entry for the same path - the
/// pool is deduplicated by path, so ten clips of one file are one entry.
class AddSample final : public Command {
public:
    explicit AddSample(std::string path);

    void apply(Project& project) override;
    void revert(Project& project) override;
    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] CommandId kind() const noexcept override;
    [[nodiscard]] DirtyMask dirty() const noexcept override;

    [[nodiscard]] core::SampleId created() const noexcept {
        return m_id;
    }

private:
    std::string m_path;
    core::SampleId m_id;
    IdMarks m_marks;
    bool m_inserted{false};
};

} // namespace adx::project
