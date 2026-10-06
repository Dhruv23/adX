// A command that has been described in Python but not yet built, and the name
// lookups every command builder needs.
#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include "engine/project/Project.h"
#include "engine/project/commands/Command.h"

namespace adx::bindings {

/// Built at execute() time rather than at construction, because building one needs
/// the project - `MoveNotes("Verse", "Lead", 120)` has to turn two names into two ids,
/// and only the handle it is executed against can do that.
struct PendingCommand {
    std::function<std::unique_ptr<project::Command>(const project::Project&)> build;
    std::string description;
    bool used{false};
};

[[nodiscard]] inline const project::Channel& channelNamed(const project::Project& project,
                                                          const std::string& name) {
    const project::Channel* channel = project.findChannelByName(name);
    if (channel == nullptr) {
        throw std::invalid_argument("no channel named '" + name + "'");
    }
    return *channel;
}

[[nodiscard]] inline const project::Pattern& patternNamed(const project::Project& project,
                                                          const std::string& name) {
    const project::Pattern* pattern = project.findPatternByName(name);
    if (pattern == nullptr) {
        throw std::invalid_argument("no pattern named '" + name + "'");
    }
    return *pattern;
}

} // namespace adx::bindings
