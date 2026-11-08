// adx-thread: realtime
//
// Follow actions (phase_11.md §4.1): what a clip does when it has played its length -
// for a looping clip, after `followRepeats` passes. The answer is a scene row in the
// clip's own track, chosen without allocation and, for Random, reproducibly from the
// project's seed.
#pragma once

#include <cstdint>

namespace adx::session {

enum class FollowAction : std::uint8_t { None, Stop, Next, Previous, First, Random, RepeatN };

/// What follows: launch `row` (in the same track), stop, or keep playing.
struct FollowResult {
    enum class Kind : std::uint8_t { Continue, Stop, Launch } kind{Kind::Continue};
    std::uint32_t row{0};
};

/// For a clip at `row` of `rows` rows, after it has completed `passes` passes. RepeatN
/// launches the clip itself again until `repeats` passes are done, then stops; Random
/// draws from `seed` (the project seed mixed with the clip and pass, by the caller) and
/// never picks the same row when there is another.
[[nodiscard]] FollowResult follow(FollowAction action, std::uint32_t row, std::uint32_t rows,
                                  std::uint32_t passes, std::uint32_t repeats,
                                  std::uint32_t seed) noexcept;

[[nodiscard]] const char* toString(FollowAction action) noexcept;
bool parseFollow(const char* text, FollowAction& out) noexcept;

} // namespace adx::session
