// A command that has been described in Python but not yet built, and the name
// lookups every command builder needs.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
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

/// A colour as `#rrggbb`.
[[nodiscard]] inline std::string colorText(const project::Color& color) {
    std::array<char, 8> text{};
    static_cast<void>(std::snprintf(text.data(), text.size(), "#%02x%02x%02x", color.red,
                                    color.green, color.blue));
    return {text.data()};
}

/// `#rrggbb` as a colour, or `fallback` for anything else (an empty string included).
[[nodiscard]] inline project::Color parseColorText(const std::string& text,
                                                   project::Color fallback) noexcept {
    if (text.size() != 7 || text[0] != '#') {
        return fallback;
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::array<int, 3> parts{};
    for (std::size_t i = 0; i < 3; ++i) {
        const int high = nibble(text[1 + (2 * i)]);
        const int low = nibble(text[2 + (2 * i)]);
        if (high < 0 || low < 0) {
            return fallback;
        }
        parts.at(i) = (high * 16) + low;
    }
    return project::Color{.red = static_cast<std::uint8_t>(parts[0]),
                          .green = static_cast<std::uint8_t>(parts[1]),
                          .blue = static_cast<std::uint8_t>(parts[2])};
}

} // namespace adx::bindings
