// What to do when a channel is at its polyphony limit.
//
// Its own header rather than part of Channel.h: the voice pool that acts on it runs on
// the audio thread, and Channel.h brings std::string and std::vector with it - which
// the realtime ban list would then see arriving in every node that allocates a voice.
#pragma once

#include <cstddef>
#include <cstdint>

namespace adx::project {

enum class VoiceStealMode : std::uint8_t { OldestReleased, Oldest, Quietest, None };
inline constexpr std::size_t kVoiceStealModeCount = 4;

} // namespace adx::project
