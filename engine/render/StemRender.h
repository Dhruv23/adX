// adx-thread: main
//
// Which stems an export writes (phase_8.md §3.5): one per channel, per insert, or per
// pattern. Every mode is a *partition* of what makes sound - each note and each audio
// clip belongs to exactly one stem - so a mode's stems sum to the master
// (stems_sum_to_master), and the rendering itself is GraphBuilder's StemSpec.
//
//   channels   one stem per channel that reaches the mixer, plus one per playlist
//              track that holds audio clips
//   inserts    one per insert that a channel or an audio track plays into directly;
//              everything downstream - buses, sends, the master - is applied
//   patterns   one per pattern placed in the arrangement, plus one for all audio clips
//              and one for frozen channels, whose renders cannot be split by pattern
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/graph/GraphBuilder.h"

namespace adx::project {
class Project;
}

namespace adx::render {

enum class StemMode : std::uint8_t {
    Channels,
    Inserts,
    Patterns,
};

[[nodiscard]] const char* toString(StemMode mode) noexcept;
[[nodiscard]] bool stemModeFromString(std::string_view name, StemMode& out) noexcept;

struct Stem {
    /// For the file name: the channel's, insert's or pattern's.
    std::string name;
    graph::StemSpec spec;
};

[[nodiscard]] std::vector<Stem> stemsOf(const project::Project& project, StemMode mode);

} // namespace adx::render
