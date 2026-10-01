// What a time source is doing.
#pragma once

#include <cstdint>

namespace adx::transport {

enum class PlayState : std::uint8_t {
    Stopped,
    Playing,
    /// Advances exactly like Playing. Separate so Phase 8's recorder can tell whether
    /// to capture input without a second flag that could disagree with this one.
    Recording,
    /// Holds position without the "return to where play started" meaning a UI may give
    /// Stopped. The engine treats the two alike; the distinction is for the UI.
    Paused,
};

/// True for the states in which position advances.
[[nodiscard]] constexpr bool isRolling(PlayState state) noexcept {
    return state == PlayState::Playing || state == PlayState::Recording;
}

[[nodiscard]] constexpr const char* toString(PlayState state) noexcept {
    switch (state) {
    case PlayState::Stopped:
        return "stopped";
    case PlayState::Playing:
        return "playing";
    case PlayState::Recording:
        return "recording";
    case PlayState::Paused:
        return "paused";
    }
    return "stopped";
}

} // namespace adx::transport
