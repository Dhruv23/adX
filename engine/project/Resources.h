// The sample pool and the paths it holds.
//
// Paths are stored **relative to the project file** and resolved against it, which
// is FINAL_PLAN §6 rule 1: binary blobs live beside the `.adx` and are referenced,
// never embedded. A project folder therefore moves or copies as a unit.
//
// Phase 2 holds the path and checks whether the file exists. Loading the audio is
// Phase 4's.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Ids.h"

namespace adx::project {

struct SampleRef {
    core::SampleId id;
    /// As written in the file: relative, forward slashes, never absolute if it can
    /// be helped. A path is written back out exactly as it came in.
    std::string path;

    [[nodiscard]] friend bool operator==(const SampleRef&, const SampleRef&) noexcept = default;
};

struct Resources {
    std::vector<SampleRef> samples;

    /// The directory the project was loaded from, used to resolve the relative
    /// paths above. Not serialised - it is a fact about where the file is, not
    /// about what the project is, and writing it would make a project
    /// non-relocatable.
    std::string baseDirectory;

    [[nodiscard]] const SampleRef* find(core::SampleId id) const noexcept;
    /// Returns the existing entry for `path`, so the same file referenced by ten
    /// clips is one pool entry.
    [[nodiscard]] const SampleRef* findByPath(std::string_view path) const noexcept;

    [[nodiscard]] friend bool operator==(const Resources& lhs, const Resources& rhs) noexcept {
        // baseDirectory is deliberately excluded: two projects loaded from
        // different directories with identical content are equal, and the undo gate
        // depends on that being true.
        return lhs.samples == rhs.samples;
    }
};

} // namespace adx::project
