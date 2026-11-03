// adx-thread: main
//
// Freeze (phase_8.md §3.4): render a channel's output to a file once, play the file in
// its place, keep everything else so thawing is exact.
//
// The render is the offline engine's, tapped at the channel's own node - its
// instrument, its volume and pan and their automation, before any insert - from the
// start of the arrangement until its last voice has released. Played back it is one
// audio region at unit rate from sample 0, which is a straight copy, so a frozen
// channel sounds bit for bit like the live one (freeze_is_transparent). What the
// render cannot know about - a later tempo change, a moved note - is what freezing
// means; the channel is frozen until it is thawed.
//
// Rendered unmuted and unsoloed whatever the project's mixer state, so a channel muted
// when it was frozen is not frozen silent; the frozen node still honours its mute.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "engine/core/Ids.h"

namespace adx::project {
class Project;
class CommandStack;
} // namespace adx::project

namespace adx::clip {

struct FreezeResult {
    bool ok{false};
    std::string error;
    std::filesystem::path file;
    std::uint64_t frames{0};
    /// RenderHash of what was written, hex.
    std::string hash;
};

/// Renders `channel` to a 32-bit float WAV at `file`: from tick 0 to the end of the
/// arrangement, then on until it is exactly silent or `maxTailSeconds` have passed.
[[nodiscard]] FreezeResult renderFreeze(const project::Project& project, core::ChannelId channel,
                                        const std::filesystem::path& file,
                                        std::uint32_t sampleRate = 48000,
                                        std::uint32_t blockFrames = 256,
                                        double maxTailSeconds = 10.0);

/// Renders into `<project directory>/freeze/<channel name>-<hash>.wav` and points the
/// channel at it, as one undo step. Named by content, so a re-freeze after an edit is a
/// new file and never the sample pool's cached decode of the old one. The file stays
/// when the freeze is undone, so a redo is instant.
[[nodiscard]] FreezeResult freezeChannel(project::Project& project, project::CommandStack& stack,
                                         core::ChannelId channel);

/// Thaws: the channel plays its instrument again. The file is left on disk.
void unfreezeChannel(project::Project& project, project::CommandStack& stack,
                     core::ChannelId channel);

} // namespace adx::clip
