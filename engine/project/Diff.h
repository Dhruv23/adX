// Semantic diff between two projects.
//
// `adx diff` exists because a textual diff of a text format is already `git diff`.
// What adX can add is "you moved 3 notes and changed the reverb mix", which needs
// the model on both sides (phase_2.md §4.13).
//
// phase_2.md §2's manifest does not list this file; it is an addition recorded in
// that plan's §10.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adx::project {

class Project;

enum class ChangeKind : std::uint8_t { Added, Removed, Changed };

struct Change {
    ChangeKind kind{ChangeKind::Changed};
    /// What changed, in the file's own vocabulary: "channel Lead", "pattern Verse",
    /// "insert 3", "tempo", "marker Drop".
    std::string subject;
    /// One line of detail, empty when the subject says it all.
    std::string detail;
};

/// Everything that differs between `before` and `after`, in canonical section
/// order. Entities are matched the way the file addresses them - channels and
/// patterns by name, inserts and playlist tracks by id - so a rename shows up as a
/// removal and an addition, which is exactly what it is to every reference.
[[nodiscard]] std::vector<Change> diff(const Project& before, const Project& after);

[[nodiscard]] const char* toString(ChangeKind kind) noexcept;

} // namespace adx::project
